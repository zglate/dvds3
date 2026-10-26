# Changelog

## 2.0.0

Maintenance release so the plugin loads on OBS Studio 33. Requires OBS 33.0
or newer. No new features.

### Why it stopped working

OBS 33 replaced `gs_image_file_*` with `gs_image_file_ex_*` (libnsgif 1.0.0
update). The 1.x binary imported three of the removed functions, so Windows
refused to load the DLL and OBS switched the plugin off.

### Fixed

- Loads on OBS 33: the image-size probe that used `gs_image_file_*` is gone;
  the logo size is read from the image source each frame instead.
- A new source now shows the logo straight away. The default for the
  *Source* setting was the translated label "Image" instead of the real
  value, which only worked because `create` forced image mode afterwards —
  and that in turn meant a saved external source was ignored until you
  touched a setting.
- External sources link correctly after loading a scene collection. OBS
  creates every source before loading any of them, so a linked source that
  came later in the file was never found. The link is now resolved in the
  `load` step and retried once a second if the source is missing or
  deleted.
- Bouncing another source no longer attaches the colour filter to that
  source (which tinted it everywhere it appeared and corrupted the filter's
  ownership). The source is wrapped in a private scene and the filter goes
  on the wrapper. Your own sources are never modified.
- *Reset* placed the logo using the width for both axes.
- A logo as large as or larger than the box no longer hangs OBS (integer
  divide by zero in the random placement) or flips direction every frame
  (strobing colour). Position is clamped to the box every frame.
- Changing a setting no longer leaks show/activate references on the
  private image source.
- The DVD source, its image source and its colour filter each have their
  own settings; previously they shared one object, which wrote
  `hue_shift` and filter defaults into the scene collection file.
- Thread safety: settings changes (UI thread) and animation (video thread)
  are now serialised with a mutex, and child sources are only linked and
  unlinked on the video thread.
- Heap overflow: external source names were concatenated into a fixed
  512-byte buffer. Replaced with `dstr`.
- `const`-correctness and unused-parameter warnings, based on upstream
  PR #3 by Ayowel.

### Changed

- Build system replaced with the official obs-plugintemplate, pinned to
  OBS 33.0.0 and obs-deps 2026-10-23. Windows x64, macOS universal and
  Ubuntu builds come from CI on every tag.
- Installs into OBS 33's plugin layout (`ProgramData\obs-studio\plugins\
  dvd-screensaver\` on Windows, `obs-modules/plugins` on Linux). The old
  `Program Files` layout stops loading in OBS 34.
- Windows ships as a zip instead of an installer. The installer in 1.x
  failed on launch because it looked for a license file by the wrong name
  (upstream issue #2).
- Linux packages are built in CI (upstream issue #1).
- `manifest.json` added so the Plugin Manager shows a name and version.
- The dynamic minimum for *Width*/*Height* (image size + 4) is gone with the
  image probe; the fixed minimum of 25 remains.

### Unchanged on purpose

- Source id, setting keys and the `dvd-screensaver` module name, so
  existing scene collections load as-is.
- No colour shift on macOS, as in 1.x.
- External sources are still linked by name.

## 1.1 and earlier

See the upstream repository: <https://github.com/univrsal/dvds3>.
