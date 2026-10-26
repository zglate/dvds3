/* This file is part of dvds3
 * DVD screen saver source
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Original: github.com/univrsal/dvds3 (univrsal, 2019-2022).
 * Modified 2026 by zglate for OBS Studio 33 (github.com/zglate/dvds3);
 * see CHANGELOG.md for what changed.
 */

#include <obs-module.h>
#include <util/dstr.h>
#include <util/threading.h>

#include <stdlib.h>

#define blog(log_level, format, ...) \
	blog(log_level, "[dvd_source: '%s'] " format, obs_source_get_name(context->source), ##__VA_ARGS__)

#define debug(format, ...) blog(LOG_DEBUG, format, ##__VA_ARGS__)
#define info(format, ...) blog(LOG_INFO, format, ##__VA_ARGS__)
#define warn(format, ...) blog(LOG_WARNING, format, ##__VA_ARGS__)

#define FILTER_NAME "dvd-filter"
#define SOURCE_NAME "dvd-image"
#define WRAPPER_NAME "dvd-wrapper"
#define DVD_SOURCE_ID "dvds3_source"

#define S_SOURCE_CX "source_cx"
#define S_SOURCE_CY "source_cy"
#define S_SPEED "speed"
#define S_SCALE "logo_scale"
#define S_COLOR "color_shift"
#define S_RESET "reset"
#define S_SOURCE_ID "source_id"
#define S_SOURCE_IMAGE "image_source"

#define TXT(t) obs_module_text(t)
#define T_SOURCE_CX TXT("DVDSource.Width")
#define T_SOURCE_CY TXT("DVDSource.Height")
#define T_SOURCE TXT("DVDSource.Name")
#define T_SPEED TXT("DVDSource.Speed")
#define T_SCALE TXT("DVDSource.Scale")
#define T_COLOR TXT("DVDSource.Color")
#define T_RESET TXT("DVDSource.Reset")
#define T_SOURCE_ID TXT("DVDSource.Source")
#define T_SOURCE_IMAGE TXT("DVDSource.Source.Image")
#define T_EXTERNAL_SOURCE TXT("DVDSource.ExternalSource")

/* Property limits; settings are clamped to the same ranges on load. */
#define MIN_CANVAS 25
#define MAX_CANVAS 0xffff
#define MIN_SPEED 0.1
#define MAX_SPEED 1000.0
#define MIN_SCALE 0.1
#define MAX_SCALE 10.0

/* Degrees of hue added on every bounce, wrapping like the original. */
#define HUE_STEP 45.0
#define HUE_MAX 180.0

/* How often (seconds) to retry resolving an external source that does not
 * exist yet, and the slower rate used after a refusal (recursion), in case
 * the user untangles it. */
#define RELINK_INTERVAL 1.0f
#define REFUSED_RETRY_INTERVAL 10.0f

/* Threading
 *
 * libobs defers update() for video sources to the video thread (it runs at
 * the start of our video tick), so update(), tick() and render() all run on
 * the video thread; create() and destroy() run with nothing else touching
 * the context. The callbacks that can arrive on other threads are load()
 * and the Reset button (UI thread) and enum_active_sources (any thread,
 * including the audio thread while OBS walks source trees).
 *
 * Two rules keep this deadlock-free:
 *  - Child sources are linked and unlinked only on the video thread, and
 *    never while holding our mutex: obs_scene_add() and
 *    obs_source_add_active_child() walk source trees under OBS's own scene
 *    locks, and those walks can call our enum_active_sources, which takes
 *    the mutex. Holding the mutex across the walk would order the locks
 *    opposite to the audio thread's walk.
 *  - Everything another thread reads is read under the mutex, and the
 *    video thread publishes pointer swaps under it. The mutex is recursive
 *    because a tree walk started by this source can re-enter
 *    enum_active_sources on this same source.
 *
 * Links are only maintained while the source is showing. That also keeps
 * the first link out of scene-collection loading: OBS creates all sources,
 * then loads them (which is when scenes and groups add their items), and
 * only then shows anything. Linking earlier could wrap a group that is
 * about to contain this source, and the group would then drop our item as
 * recursive. Between a swap and the matching activate/deactivate there are
 * the same microsecond windows libobs' own scene code has.
 */
struct dvd_source {
	obs_source_t *source;
	pthread_mutex_t mutex;

	/* Private children, owned by this source. Pointers are swapped on the
	 * video thread under the mutex; other threads read them under it. */
	obs_source_t *logo_source;  /* image_source showing the logo file */
	obs_source_t *color_filter; /* color_filter doing the hue shift; NULL on macOS */
	bool logo_active;           /* logo_source is registered as an active child */

	/* External-source mode: the user's source is wrapped in a private scene so the
	 * color filter can be applied to the wrapper without touching the user's source. */
	obs_scene_t *wrapper_scene;
	obs_source_t *wrapper;         /* obs_scene_get_source(wrapper_scene), not a new ref */
	obs_sceneitem_t *wrapper_item; /* our own ref; OBS may prune the scene's */
	obs_weak_source_t *external;   /* the user's source, weak so we never keep it alive */

	/* Video-thread-only bookkeeping. */
	obs_source_t *filter_host; /* which source color_filter is attached to */
	float relink_timer;
	bool link_refused;         /* recursion: retry slowly until the setting changes */
	uint32_t base_cx, base_cy; /* OBS canvas size when the wrapper was linked */

	/* Wanted state, written by update() (video thread) and load(). */
	bool use_image;
	char *external_name;   /* name of the wanted external source, NULL in image mode */
	bool relink_requested; /* external_name changed, or load() asked for a retry */

	bool use_color;
	uint32_t cx, cy;           /* box size */
	uint32_t logo_cx, logo_cy; /* unscaled logo size as last measured by tick() */
	struct vec2 pos;           /* logo position */
	int8_t d_x, d_y;           /* direction, -1 or 1 */
	double scale, speed;       /* logo scale, pixels per second */
	double hue;                /* current hue shift in degrees */
};

struct enum_param {
	obs_property_t *list;
	obs_source_t *self;
};

static const char *dvd_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_SOURCE;
}

/* "" and the pre-2.0 translated label both mean the bundled image. */
static bool is_image_choice(const char *id)
{
	return !*id || strcmp(id, S_SOURCE_IMAGE) == 0 || strcmp(id, T_SOURCE_IMAGE) == 0;
}

/* Uniform value in [0, max]; 0 when there is no room. Avoids the integer
 * division of the original, which divided by zero for large or negative limits. */
static float rand_range(float max)
{
	if (max <= 0.0f)
		return 0.0f;
	return (float)rand() / (float)RAND_MAX * max;
}

static double clamp_double(double v, double lo, double hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t clamp_canvas(long long v)
{
	return (uint32_t)(v < MIN_CANVAS ? MIN_CANVAS : (v > MAX_CANVAS ? MAX_CANVAS : v));
}

/* ---- color filter -------------------------------------------------------- */

/* Move the private color filter to `host`. Video thread, mutex not held. */
static void attach_filter(struct dvd_source *context, obs_source_t *host)
{
	if (!context->color_filter || context->filter_host == host)
		return;
	if (context->filter_host)
		obs_source_filter_remove(context->filter_host, context->color_filter);
	if (host)
		obs_source_filter_add(host, context->color_filter);
	context->filter_host = host;
}

/* Push a hue value into the filter. Mutex not held. */
static void apply_hue(struct dvd_source *context, double hue)
{
	if (!context->color_filter)
		return;
	obs_data_t *data = obs_data_create();
	obs_data_set_double(data, "hue_shift", hue);
	obs_source_update(context->color_filter, data);
	obs_data_release(data);
}

/* ---- child source linking (video thread, mutex not held on entry) -------- */

/* Drop the wrapper scene and the weak link, if any. */
static void drop_external(struct dvd_source *context)
{
	pthread_mutex_lock(&context->mutex);
	obs_scene_t *scene = context->wrapper_scene;
	obs_source_t *wrapper = context->wrapper;
	obs_sceneitem_t *item = context->wrapper_item;
	obs_weak_source_t *weak = context->external;
	context->wrapper_scene = NULL;
	context->wrapper = NULL;
	context->wrapper_item = NULL;
	context->external = NULL;
	pthread_mutex_unlock(&context->mutex);

	if (scene) {
		if (context->filter_host == wrapper)
			attach_filter(context, NULL);
		obs_source_remove_active_child(context->source, wrapper);
		/* If the user deleted the source, OBS has already pruned the item
		 * from the scene; remove() is then a no-op and we just drop our ref. */
		obs_sceneitem_remove(item);
		obs_sceneitem_release(item);
		obs_scene_release(scene);
	}
	obs_weak_source_release(weak);
}

/* Show the logo image. */
static void show_logo(struct dvd_source *context)
{
	drop_external(context);
	if (!context->logo_active) {
		obs_source_add_active_child(context->source, context->logo_source);
		pthread_mutex_lock(&context->mutex);
		context->logo_active = true;
		pthread_mutex_unlock(&context->mutex);
	}
	attach_filter(context, context->logo_source);
}

/* Try to wrap the source named `name`. Returns false when it does not exist
 * (yet), is being deleted, or would recurse. */
static bool link_external(struct dvd_source *context, const char *name)
{
	obs_source_t *ext = obs_get_source_by_name(name);
	if (!ext)
		return false;
	if (obs_source_removed(ext)) {
		obs_source_release(ext);
		return false;
	}

	/* obs_scene_create_private() logs that the scene has no canvas; that is
	 * expected for a scene that is only ever rendered by us. */
	obs_scene_t *scene = obs_scene_create_private(WRAPPER_NAME);
	obs_source_t *wrapper = obs_scene_get_source(scene);
	obs_sceneitem_t *item = obs_scene_add(scene, ext);

	if (!item) {
		if (!context->link_refused)
			warn("Can't use '%s', adding it to the wrapper scene failed", name);
		obs_scene_release(scene);
		obs_source_release(ext);
		context->link_refused = true;
		return false;
	}
	obs_sceneitem_addref(item);

	if (!obs_source_add_active_child(context->source, wrapper)) {
		/* ext (or something inside it) contains this very source. */
		if (!context->link_refused)
			warn("Can't use '%s', it would cause recursion", name);
		obs_sceneitem_remove(item);
		obs_sceneitem_release(item);
		obs_scene_release(scene);
		obs_source_release(ext);
		context->link_refused = true;
		return false;
	}

	context->link_refused = false;

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		context->base_cx = ovi.base_width;
		context->base_cy = ovi.base_height;
	}

	if (context->logo_active) {
		pthread_mutex_lock(&context->mutex);
		context->logo_active = false;
		pthread_mutex_unlock(&context->mutex);
		obs_source_remove_active_child(context->source, context->logo_source);
	}
	attach_filter(context, wrapper);

	pthread_mutex_lock(&context->mutex);
	context->wrapper_scene = scene;
	context->wrapper = wrapper;
	context->wrapper_item = item;
	context->external = obs_source_get_weak_source(ext);
	pthread_mutex_unlock(&context->mutex);

	obs_source_release(ext);
	info("Bouncing source '%s'", name);
	return true;
}

/* Bring the linked children in line with the wanted state. */
static void maintain_links(struct dvd_source *context, float seconds)
{
	pthread_mutex_lock(&context->mutex);
	bool use_image = context->use_image;
	const char *name = context->external_name;
	bool relink = context->relink_requested;
	bool linked = context->wrapper_scene != NULL;
	obs_source_t *ext = context->external ? obs_weak_source_get_source(context->external) : NULL;
	context->relink_requested = false;
	pthread_mutex_unlock(&context->mutex);

	if (use_image) {
		if (!context->logo_active || linked)
			show_logo(context);
		context->link_refused = false;
		obs_source_release(ext);
		return;
	}

	if (relink) {
		drop_external(context);
		linked = false;
		context->link_refused = false;
		context->relink_timer = RELINK_INTERVAL; /* try right away */
	} else if (linked && (!ext || obs_source_removed(ext))) {
		/* The user deleted the source we were bouncing. Keep retrying by
		 * name so a source recreated with that name is picked up again. */
		info("Source '%s' went away", name);
		drop_external(context);
		linked = false;
		context->relink_timer = 0.0f;
	} else if (linked) {
		/* The wrapper's item is placed relative to the OBS canvas, so a
		 * resolution change moves and scales it. Relink to re-derive. */
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi) &&
		    (ovi.base_width != context->base_cx || ovi.base_height != context->base_cy)) {
			drop_external(context);
			linked = false;
			context->relink_timer = RELINK_INTERVAL;
		}
	}
	obs_source_release(ext);

	if (!linked && name) {
		float interval = context->link_refused ? REFUSED_RETRY_INTERVAL : RELINK_INTERVAL;
		context->relink_timer += seconds;
		if (context->relink_timer >= interval) {
			context->relink_timer = 0.0f;
			link_external(context, name);
		}
	}
}

/* ---- shared queries (mutex held, any thread) ----------------------------- */

/* Strong ref to whatever should be drawn, or NULL. Caller releases. */
static obs_source_t *get_render_target(struct dvd_source *context)
{
	obs_source_t *target = context->use_image ? context->logo_source : context->wrapper;
	return target ? obs_source_get_ref(target) : NULL;
}

/* Measure the unscaled logo size as drawn, i.e. after the source's own
 * filters, and cache it for reset_logo_position(). 0x0 when nothing is
 * loaded. Video thread, mutex not held: another plugin's get_width could
 * take locks we must not nest under ours. */
static void measure_logo(struct dvd_source *context)
{
	pthread_mutex_lock(&context->mutex);
	obs_source_t *target = context->use_image
				       ? obs_source_get_ref(context->logo_source)
				       : (context->external ? obs_weak_source_get_source(context->external) : NULL);
	pthread_mutex_unlock(&context->mutex);

	uint32_t cx = 0, cy = 0;
	if (target) {
		cx = obs_source_get_width(target);
		cy = obs_source_get_height(target);
		obs_source_release(target);
	}

	pthread_mutex_lock(&context->mutex);
	context->logo_cx = cx;
	context->logo_cy = cy;
	pthread_mutex_unlock(&context->mutex);
}

/* ---- settings ------------------------------------------------------------ */

static void dvd_source_update(void *data, obs_data_t *settings)
{
	struct dvd_source *context = data;

	if (context->color_filter)
		obs_source_set_enabled(context->color_filter, obs_data_get_bool(settings, S_COLOR));

	/* The logo image source keeps its own settings; hand over the keys it reads. */
	obs_data_t *logo_settings = obs_data_create();
	obs_data_set_string(logo_settings, "file", obs_data_get_string(settings, "file"));
	obs_data_set_bool(logo_settings, "unload", obs_data_get_bool(settings, "unload"));
	obs_data_set_bool(logo_settings, "linear_alpha", obs_data_get_bool(settings, "linear_alpha"));
	obs_source_update(context->logo_source, logo_settings);
	obs_data_release(logo_settings);

	/* Which source to bounce. External sources are stored as their name plus an
	 * 'e' so a source literally called "image_source" cannot collide with the
	 * built-in option. Versions before 2.0 defaulted this setting to the
	 * translated "Image" label by mistake; migrate that to the real value. */
	const char *id = obs_data_get_string(settings, S_SOURCE_ID);
	bool use_image = is_image_choice(id);
	if (use_image && strcmp(id, S_SOURCE_IMAGE) != 0)
		obs_data_set_string(settings, S_SOURCE_ID, S_SOURCE_IMAGE);

	char *requested = NULL;
	if (!use_image) {
		requested = bstrdup(id);
		size_t len = strlen(requested);
		if (len > 1)
			requested[len - 1] = '\0';
	}

	pthread_mutex_lock(&context->mutex);

	context->cx = clamp_canvas(obs_data_get_int(settings, S_SOURCE_CX));
	context->cy = clamp_canvas(obs_data_get_int(settings, S_SOURCE_CY));
	context->scale = clamp_double(obs_data_get_double(settings, S_SCALE), MIN_SCALE, MAX_SCALE);
	context->speed = clamp_double(obs_data_get_double(settings, S_SPEED), MIN_SPEED, MAX_SPEED);
	context->use_color = obs_data_get_bool(settings, S_COLOR);

	bool same_name = requested && context->external_name && strcmp(requested, context->external_name) == 0;
	if (use_image != context->use_image || (requested && !same_name)) {
		context->use_image = use_image;
		bfree(context->external_name);
		context->external_name = requested;
		requested = NULL;
		context->relink_requested = true;
	}

	pthread_mutex_unlock(&context->mutex);

	bfree(requested);
}

/* Scene collections create every source before any of them is loaded, so an
 * external source may not have existed when create() ran update(). If it is
 * still unlinked by now, ask tick() to try again at once. */
static void dvd_source_load(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	struct dvd_source *context = data;

	pthread_mutex_lock(&context->mutex);
	if (!context->use_image && !context->wrapper_scene)
		context->relink_requested = true;
	pthread_mutex_unlock(&context->mutex);
}

static void dvd_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_SOURCE_CX, 640);
	obs_data_set_default_int(settings, S_SOURCE_CY, 480);
	obs_data_set_default_double(settings, S_SCALE, 1.0);
	obs_data_set_default_double(settings, S_SPEED, 50.0);

	obs_data_set_default_bool(settings, S_COLOR, true);
	char *path = obs_module_file("dvd.png");
	obs_data_set_default_string(settings, "file", path);
	obs_data_set_default_string(settings, S_SOURCE_ID, S_SOURCE_IMAGE);
	bfree(path);
}

/* ---- lifecycle ----------------------------------------------------------- */

static void *dvd_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct dvd_source *context = bzalloc(sizeof(struct dvd_source));
	context->source = source;
	pthread_mutex_init_recursive(&context->mutex);

	/* Children get their own settings; sharing this source's settings object
	 * (as 1.x did) leaked the filter's hue into the saved scene. */
	context->logo_source = obs_source_create_private("image_source", SOURCE_NAME, NULL);
#ifndef __APPLE__
	/* Color shifting has never been enabled on macOS in this plugin; kept that way. */
	context->color_filter = obs_source_create_private("color_filter", FILTER_NAME, NULL);
#endif

	context->use_image = true;
	context->d_x = 1;
	context->d_y = 1;

	show_logo(context);
	dvd_source_update(context, settings);
	return context;
}

static void dvd_source_destroy(void *data)
{
	struct dvd_source *context = data;

	drop_external(context);
	attach_filter(context, NULL); /* detach from the logo */
	if (context->logo_active)
		obs_source_remove_active_child(context->source, context->logo_source);

	obs_source_release(context->color_filter);
	obs_source_release(context->logo_source);
	bfree(context->external_name);
	pthread_mutex_destroy(&context->mutex);
	bfree(context);
}

/* Called from any thread (scene item transforms, UI), so read under the mutex. */
static uint32_t dvd_source_get_width(void *data)
{
	struct dvd_source *context = data;
	pthread_mutex_lock(&context->mutex);
	uint32_t cx = context->cx;
	pthread_mutex_unlock(&context->mutex);
	return cx;
}

static uint32_t dvd_source_get_height(void *data)
{
	struct dvd_source *context = data;
	pthread_mutex_lock(&context->mutex);
	uint32_t cy = context->cy;
	pthread_mutex_unlock(&context->mutex);
	return cy;
}

/* ---- per-frame (video thread) -------------------------------------------- */

static void dvd_source_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct dvd_source *context = data;

	pthread_mutex_lock(&context->mutex);
	obs_source_t *target = get_render_target(context);
	struct vec2 pos = context->pos;
	float scale = (float)context->scale;
	pthread_mutex_unlock(&context->mutex);

	if (!target)
		return;

	gs_matrix_push();
	gs_matrix_translate3f(pos.x, pos.y, 0.f);
	gs_matrix_scale3f(scale, scale, 1.f);
	obs_source_video_render(target);
	gs_matrix_pop();

	obs_source_release(target);
}

/* Move one axis. Clamps to the canvas so a logo that is larger than the canvas,
 * or a canvas shrunk underneath the logo, sits at the edge instead of flipping
 * direction every frame. Returns true on a bounce. */
static bool step_axis(float *pos, int8_t *dir, float delta, float room)
{
	if (room <= 0.0f) {
		*pos = 0.0f;
		return false;
	}

	float next = *pos + delta;
	bool bounce = false;

	if (next <= 0.0f) {
		next = 0.0f;
		*dir = 1;
		bounce = true;
	} else if (next >= room) {
		next = room;
		*dir = -1;
		bounce = true;
	}
	*pos = next;
	return bounce;
}

static void dvd_source_tick(void *data, float seconds)
{
	struct dvd_source *context = data;
	bool bounce = false;
	double hue = 0.0;

	if (!obs_source_showing(context->source)) {
		/* Links are only maintained while showing, but a source the user
		 * deleted must not be kept alive by our wrapper item meanwhile. */
		pthread_mutex_lock(&context->mutex);
		bool linked = context->wrapper_scene != NULL;
		obs_source_t *ext = context->external ? obs_weak_source_get_source(context->external) : NULL;
		pthread_mutex_unlock(&context->mutex);
		if (linked && (!ext || obs_source_removed(ext))) {
			drop_external(context);
			context->relink_timer = 0.0f;
		}
		obs_source_release(ext);
		return;
	}

	maintain_links(context, seconds);
	measure_logo(context);

	pthread_mutex_lock(&context->mutex);

	if (context->logo_cx > 0 && context->logo_cy > 0) {
		float w = (float)(context->logo_cx * context->scale);
		float h = (float)(context->logo_cy * context->scale);
		float step = seconds * (float)context->speed;

		bounce |= step_axis(&context->pos.x, &context->d_x, context->d_x * step, (float)context->cx - w);
		bounce |= step_axis(&context->pos.y, &context->d_y, context->d_y * step, (float)context->cy - h);

		/* The hue keeps advancing while color shift is off (the filter is
		 * just disabled), so turning it back on resumes where 1.x would. */
		if (bounce && context->color_filter) {
			context->hue += HUE_STEP;
			if (context->hue > HUE_MAX)
				context->hue = -HUE_MAX + HUE_STEP;
			hue = context->hue;
		} else {
			bounce = false;
		}
	}

	pthread_mutex_unlock(&context->mutex);

	if (bounce)
		apply_hue(context, hue);
}

/* ---- properties (UI thread) ---------------------------------------------- */

static bool reset_logo_position(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct dvd_source *context = data;

	pthread_mutex_lock(&context->mutex);
	float w = (float)(context->logo_cx * context->scale);
	float h = (float)(context->logo_cy * context->scale);
	context->pos.x = rand_range((float)context->cx - w);
	context->pos.y = rand_range((float)context->cy - h);
	context->d_x = (rand() & 1) ? -1 : 1;
	context->d_y = (rand() & 1) ? -1 : 1;
	pthread_mutex_unlock(&context->mutex);
	return true;
}

static bool add_source_to_list(void *param, obs_source_t *current)
{
	struct enum_param *data = param;
	uint32_t flags = obs_source_get_output_flags(current);
	const char *name = obs_source_get_name(current);
	const char *id = obs_source_get_id(current);

	if (data->self == current || (flags & OBS_SOURCE_VIDEO) == 0 || !name || !id ||
	    strcmp(name, SOURCE_NAME) == 0 || strcmp(id, DVD_SOURCE_ID) == 0)
		return true;

	struct dstr label = {0};
	struct dstr value = {0};
	dstr_printf(&label, "%s %s", T_EXTERNAL_SOURCE, name);
	/* Postfix so an external source can never be confused with the built-in
	 * S_SOURCE_IMAGE option; dvd_source_update() strips it again. */
	dstr_printf(&value, "%se", name);
	obs_property_list_add_string(data->list, label.array, value.array);
	dstr_free(&label);
	dstr_free(&value);
	return true;
}

static bool source_choice_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *data)
{
	UNUSED_PARAMETER(prop);
	bool image = is_image_choice(obs_data_get_string(data, S_SOURCE_ID));
	obs_property_set_visible(obs_properties_get(props, "file"), image);
	return true;
}

static obs_properties_t *dvd_source_get_properties(void *data)
{
	struct dvd_source *context = data;
	if (!context)
		return NULL;

	/* Start from the image source's own properties so the file picker matches
	 * whatever OBS offers for images. */
	obs_properties_t *props = obs_source_properties(context->logo_source);
	if (!props)
		return NULL;

	obs_property_t *list =
		obs_properties_add_list(props, S_SOURCE_ID, T_SOURCE_ID, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, T_SOURCE_IMAGE, S_SOURCE_IMAGE);
	struct enum_param e = {.list = list, .self = context->source};
	obs_property_set_modified_callback(list, source_choice_changed);
	obs_enum_sources(add_source_to_list, &e);

	if (context->color_filter)
		obs_properties_add_bool(props, S_COLOR, T_COLOR);
	obs_properties_add_int(props, S_SOURCE_CX, T_SOURCE_CX, MIN_CANVAS, MAX_CANVAS, 1);
	obs_properties_add_int(props, S_SOURCE_CY, T_SOURCE_CY, MIN_CANVAS, MAX_CANVAS, 1);
	obs_properties_add_float_slider(props, S_SPEED, T_SPEED, MIN_SPEED, MAX_SPEED, 0.25);
	obs_properties_add_float_slider(props, S_SCALE, T_SCALE, MIN_SCALE, MAX_SCALE, 0.25);
	obs_properties_add_button2(props, S_RESET, T_RESET, reset_logo_position, context);

	obs_property_set_visible(obs_properties_get(props, "unload"), false);

	return props;
}

/* ---- source tree (any thread) -------------------------------------------- */

static void dvd_source_enum_active(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct dvd_source *context = data;

	pthread_mutex_lock(&context->mutex);
	obs_source_t *child = context->logo_active ? context->logo_source : context->wrapper;
	child = child ? obs_source_get_ref(child) : NULL;
	pthread_mutex_unlock(&context->mutex);

	if (child) {
		cb(context->source, child, param);
		obs_source_release(child);
	}
}

static struct obs_source_info dvd_source_info = {
	.id = DVD_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = dvd_source_get_name,
	.create = dvd_source_create,
	.destroy = dvd_source_destroy,
	.update = dvd_source_update,
	.load = dvd_source_load,
	.get_defaults = dvd_source_defaults,
	.get_width = dvd_source_get_width,
	.get_height = dvd_source_get_height,
	.video_render = dvd_source_render,
	.enum_active_sources = dvd_source_enum_active,
	.video_tick = dvd_source_tick,
	.get_properties = dvd_source_get_properties,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("dvd-screensaver", "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "DVD screen saver";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Bounces a logo around the canvas, DVD-player style, shifting color on every bounce.";
}

bool obs_module_load(void)
{
	obs_register_source(&dvd_source_info);
	return true;
}
