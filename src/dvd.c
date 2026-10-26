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
 * github.com/univrsal/dvds3 (original), github.com/zglate/dvds3 (this fork)
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

#define _T(t) obs_module_text(t)
#define T_SOURCE_CX _T("DVDSource.Width")
#define T_SOURCE_CY _T("DVDSource.Height")
#define T_SOURCE _T("DVDSource.Name")
#define T_SPEED _T("DVDSource.Speed")
#define T_SCALE _T("DVDSource.Scale")
#define T_COLOR _T("DVDSource.Color")
#define T_RESET _T("DVDSource.Reset")
#define T_SOURCE_ID _T("DVDSource.Source")
#define T_SOURCE_IMAGE _T("DVDSource.Source.Image")
#define T_EXTERNAL_SOURCE _T("DVDSource.ExternalSource")

/* Degrees of hue added on every bounce, wrapping like the original. */
#define HUE_STEP 45.0
#define HUE_MAX 180.0

/* How often (seconds) to retry resolving an external source that does not
 * exist yet, e.g. because it comes later in the scene collection. */
#define RELINK_INTERVAL 1.0f

/* Threading
 *
 * update()/load()/the Reset button run on the UI thread; tick()/render() run on
 * the video thread, which holds the graphics lock. Calling into child sources
 * (add_active_child, obs_source_update on the image source, ...) can take the
 * graphics lock, so doing that on the UI thread while holding our mutex would
 * deadlock against the video thread waiting for the same mutex.
 *
 * Rule: the UI thread only ever touches plain fields under the mutex. Every
 * OBS call that creates, links or unlinks child sources happens on the video
 * thread, in tick(), driven by the "wanted" state that update() records.
 */
struct dvd_source {
	obs_source_t *source;

	/* Private children, owned by this source. */
	obs_source_t *logo_source;  /* image_source showing the logo file */
	obs_source_t *color_filter; /* color_filter doing the hue shift; NULL on macOS */
	obs_source_t *filter_host;  /* which source color_filter is currently attached to */
	bool logo_active;           /* logo_source is registered as an active child */

	/* External-source mode: the user's source is wrapped in a private scene so the
	 * colour filter can be applied to the wrapper without touching the user's source. */
	obs_scene_t *wrapper_scene;
	obs_source_t *wrapper; /* obs_scene_get_source(wrapper_scene), not a new ref */
	obs_sceneitem_t *wrapper_item;
	obs_weak_source_t *external; /* the user's source, weak so we never keep it alive */
	float relink_timer;

	pthread_mutex_t mutex; /* recursive; guards everything below */

	/* Wanted state, written by update()/load(), acted on by tick(). */
	bool use_image;
	char *external_name;   /* name of the wanted external source, NULL in image mode */
	bool relink_requested; /* external_name changed or load() asked for a retry */

	bool use_color;
	uint32_t cx, cy;     /* canvas size */
	struct vec2 pos;     /* logo position */
	int8_t d_x, d_y;     /* direction, -1 or 1 */
	double scale, speed; /* logo scale, pixels per second */
	double hue;          /* current hue shift in degrees */
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

/* Uniform value in [0, max]; 0 when there is no room. Avoids the integer
 * division of the original, which divided by zero for large or negative limits. */
static float rand_range(float max)
{
	if (max <= 0.0f)
		return 0.0f;
	return (float)rand() / (float)RAND_MAX * max;
}

/* ---- colour filter ------------------------------------------------------ */

/* Move the private colour filter to `host`. Video thread. */
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

/* Push a hue value into the filter. Called without the mutex held. */
static void apply_hue(struct dvd_source *context, double hue)
{
	if (!context->color_filter)
		return;
	obs_data_t *data = obs_data_create();
	obs_data_set_double(data, "hue_shift", hue);
	obs_source_update(context->color_filter, data);
	obs_data_release(data);
}

/* ---- child source linking (video thread, mutex held) ---------------------- */

/* Drop the wrapper scene and the weak link. */
static void unlink_external(struct dvd_source *context)
{
	if (context->wrapper_scene) {
		if (context->filter_host == context->wrapper)
			attach_filter(context, NULL);
		obs_source_remove_active_child(context->source, context->wrapper);
		obs_sceneitem_remove(context->wrapper_item);
		obs_scene_release(context->wrapper_scene);
		context->wrapper_scene = NULL;
		context->wrapper = NULL;
		context->wrapper_item = NULL;
	}
	if (context->external) {
		obs_weak_source_release(context->external);
		context->external = NULL;
	}
}

/* Show the logo image. */
static void use_logo(struct dvd_source *context)
{
	unlink_external(context);
	if (!context->logo_active) {
		obs_source_add_active_child(context->source, context->logo_source);
		context->logo_active = true;
	}
	attach_filter(context, context->logo_source);
}

/* Try to wrap the source named context->external_name. Returns false when it
 * does not exist (yet) or would recurse. */
static bool link_external(struct dvd_source *context)
{
	obs_source_t *ext = obs_get_source_by_name(context->external_name);
	if (!ext)
		return false;

	bool ok = false;
	obs_scene_t *scene = obs_scene_create_private(WRAPPER_NAME);
	obs_source_t *wrapper = obs_scene_get_source(scene);
	/* obs_scene_add refuses a source whose tree contains the scene. */
	obs_sceneitem_t *item = obs_scene_add(scene, ext);

	if (!item) {
		warn("Can't use '%s', adding it to the wrapper scene failed", context->external_name);
	} else if (!obs_source_add_active_child(context->source, wrapper)) {
		/* ext (or something inside it) contains this very source. */
		warn("Can't use '%s', it would cause recursion", context->external_name);
		obs_sceneitem_remove(item);
	} else {
		ok = true;
	}

	if (!ok) {
		obs_scene_release(scene);
		obs_source_release(ext);
		return false;
	}

	if (context->logo_active) {
		obs_source_remove_active_child(context->source, context->logo_source);
		context->logo_active = false;
	}
	context->wrapper_scene = scene;
	context->wrapper = wrapper;
	context->wrapper_item = item;
	context->external = obs_source_get_weak_source(ext);
	attach_filter(context, wrapper);
	obs_source_release(ext);
	info("Bouncing source '%s'", context->external_name);
	return true;
}

/* Bring the linked children in line with the wanted state. */
static void maintain_links(struct dvd_source *context, float seconds)
{
	if (context->use_image) {
		if (!context->logo_active || context->wrapper_scene)
			use_logo(context);
		context->relink_requested = false;
		return;
	}

	if (context->relink_requested) {
		unlink_external(context);
		context->relink_requested = false;
		context->relink_timer = RELINK_INTERVAL; /* try right away */
	}

	/* The user deleted the source we were bouncing: fall back to retrying by
	 * name, which picks it up again if a source with that name reappears. */
	if (context->external) {
		obs_source_t *ext = obs_weak_source_get_source(context->external);
		bool gone = !ext || obs_source_removed(ext);
		obs_source_release(ext);
		if (gone) {
			info("Source '%s' went away", context->external_name);
			unlink_external(context);
		}
	}

	if (!context->wrapper_scene && context->external_name) {
		context->relink_timer += seconds;
		if (context->relink_timer >= RELINK_INTERVAL) {
			context->relink_timer = 0.0f;
			link_external(context);
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

/* Unscaled size of the logo (image or external source). 0x0 when nothing is loaded. */
static void get_logo_size(struct dvd_source *context, uint32_t *cx, uint32_t *cy)
{
	*cx = 0;
	*cy = 0;
	if (context->use_image) {
		*cx = obs_source_get_base_width(context->logo_source);
		*cy = obs_source_get_base_height(context->logo_source);
	} else if (context->external) {
		obs_source_t *ext = obs_weak_source_get_source(context->external);
		if (ext) {
			*cx = obs_source_get_base_width(ext);
			*cy = obs_source_get_base_height(ext);
			obs_source_release(ext);
		}
	}
}

/* ---- settings (UI thread) ------------------------------------------------ */

static void dvd_source_update(void *data, obs_data_t *settings)
{
	struct dvd_source *context = data;

	/* Child sources first, outside the mutex (see the threading note above). */
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
	 * translated "Image" label by mistake, so that value also means the image. */
	const char *id = obs_data_get_string(settings, S_SOURCE_ID);
	bool use_image = !id || !*id || strcmp(id, S_SOURCE_IMAGE) == 0 || strcmp(id, T_SOURCE_IMAGE) == 0;

	char *requested = NULL;
	if (!use_image) {
		requested = bstrdup(id);
		size_t len = strlen(requested);
		if (len > 1)
			requested[len - 1] = '\0';
	}

	pthread_mutex_lock(&context->mutex);

	context->cx = (uint32_t)obs_data_get_int(settings, S_SOURCE_CX);
	context->cy = (uint32_t)obs_data_get_int(settings, S_SOURCE_CY);
	context->scale = obs_data_get_double(settings, S_SCALE);
	context->speed = obs_data_get_double(settings, S_SPEED);
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
 * external source may not exist when update() first runs. Ask tick() to retry. */
static void dvd_source_load(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	struct dvd_source *context = data;

	pthread_mutex_lock(&context->mutex);
	if (!context->use_image)
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
	 * (as 1.x did) leaked the filter's keys into the saved scene. */
	context->logo_source = obs_source_create_private("image_source", SOURCE_NAME, NULL);
#ifndef __APPLE__
	/* Colour shifting has never been enabled on macOS in this plugin; kept that way. */
	context->color_filter = obs_source_create_private("color_filter", FILTER_NAME, NULL);
#endif

	context->use_image = true;
	context->d_x = 1;
	context->d_y = 1;

	/* Nothing else can see the context yet, so linking here is safe. */
	use_logo(context);

	dvd_source_update(context, settings);
	return context;
}

static void dvd_source_destroy(void *data)
{
	struct dvd_source *context = data;

	/* OBS has already taken the source out of the tick/render lists. */
	unlink_external(context);
	attach_filter(context, NULL);
	if (context->logo_active) {
		obs_source_remove_active_child(context->source, context->logo_source);
		context->logo_active = false;
	}

	obs_source_release(context->color_filter);
	obs_source_release(context->logo_source);
	bfree(context->external_name);
	pthread_mutex_destroy(&context->mutex);
	bfree(context);
}

static uint32_t dvd_source_getwidth(void *data)
{
	struct dvd_source *context = data;
	return context->cx;
}

static uint32_t dvd_source_getheight(void *data)
{
	struct dvd_source *context = data;
	return context->cy;
}

/* ---- per-frame (video thread) -------------------------------------------- */

static void dvd_source_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct dvd_source *context = data;

	if (!obs_source_showing(context->source))
		return;

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

	pthread_mutex_lock(&context->mutex);

	maintain_links(context, seconds);

	if (obs_source_showing(context->source)) {
		uint32_t logo_cx, logo_cy;
		get_logo_size(context, &logo_cx, &logo_cy);

		if (logo_cx > 0 && logo_cy > 0) {
			float w = (float)(logo_cx * context->scale);
			float h = (float)(logo_cy * context->scale);
			float step = seconds * (float)context->speed;

			bounce |=
				step_axis(&context->pos.x, &context->d_x, context->d_x * step, (float)context->cx - w);
			bounce |=
				step_axis(&context->pos.y, &context->d_y, context->d_y * step, (float)context->cy - h);

			if (bounce && context->use_color && context->color_filter) {
				context->hue += HUE_STEP;
				if (context->hue > HUE_MAX)
					context->hue = -HUE_MAX + HUE_STEP;
				hue = context->hue;
			} else {
				bounce = false;
			}
		}
	}

	pthread_mutex_unlock(&context->mutex);

	if (bounce)
		apply_hue(context, hue);
}

/* ---- properties (UI thread) ---------------------------------------------- */

static bool use_color_shift_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(data);
	/* Returning true forces an update */
	return true;
}

static bool reset_logo_position(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct dvd_source *context = data;

	pthread_mutex_lock(&context->mutex);
	uint32_t logo_cx, logo_cy;
	get_logo_size(context, &logo_cx, &logo_cy);
	float w = (float)(logo_cx * context->scale);
	float h = (float)(logo_cy * context->scale);
	context->pos.x = rand_range((float)context->cx - w);
	context->pos.y = rand_range((float)context->cy - h);
	context->d_x = (rand() & 1) ? -1 : 1;
	context->d_y = (rand() & 1) ? -1 : 1;
	pthread_mutex_unlock(&context->mutex);
	return true;
}

static bool dvd_enum_global_sources(void *param, obs_source_t *current)
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

static bool source_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *data)
{
	UNUSED_PARAMETER(prop);
	const char *val = obs_data_get_string(data, S_SOURCE_ID);
	bool vis = strcmp(val, S_SOURCE_IMAGE) == 0;
	obs_property_t *path = obs_properties_get(props, "file");

	obs_property_set_visible(path, vis);
	return true;
}

static obs_properties_t *dvd_source_properties(void *data)
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
	obs_property_set_modified_callback(list, source_changed);
	obs_enum_sources(dvd_enum_global_sources, &e);

	if (context->color_filter) {
		obs_property_t *color = obs_properties_add_bool(props, S_COLOR, T_COLOR);
		obs_property_set_modified_callback(color, use_color_shift_changed);
	}
	obs_properties_add_int(props, S_SOURCE_CX, T_SOURCE_CX, 25, 0xffff, 1);
	obs_properties_add_int(props, S_SOURCE_CY, T_SOURCE_CY, 25, 0xffff, 1);
	obs_properties_add_float_slider(props, S_SPEED, T_SPEED, 0.10f, 1000.f, 0.25);
	obs_properties_add_float_slider(props, S_SCALE, T_SCALE, 0.10f, 10.f, 0.25);
	obs_properties_add_button2(props, S_RESET, T_RESET, reset_logo_position, context);

	obs_property_set_visible(obs_properties_get(props, "unload"), false);

	return props;
}

/* ---- source tree (any thread) -------------------------------------------- */

static void dvd_enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
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
	.get_width = dvd_source_getwidth,
	.get_height = dvd_source_getheight,
	.video_render = dvd_source_render,
	.enum_active_sources = dvd_enum_sources,
	.video_tick = dvd_source_tick,
	.get_properties = dvd_source_properties,
	.icon_type = OBS_ICON_TYPE_IMAGE,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("dvd-screensaver", "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "DVD screen saver";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Bounces a logo around the canvas, DVD-player style, shifting colour on every bounce.";
}

bool obs_module_load(void)
{
	obs_register_source(&dvd_source_info);
	return true;
}
