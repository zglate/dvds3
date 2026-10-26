# dvds3 - DVD screen saver source for OBS Studio

![logo](./data/dvd.png)

Will it hit the corner?

Adds a **DVD screen saver** source to OBS: a logo bounces around a box and
changes colour every time it hits an edge. The logo can be the bundled DVD
image, any image file, or any other video source you already have in OBS.

## Credit where it's due

**[univrsal](https://github.com/univrsal) wrote this plugin.** The idea, the
design and all of the original code are theirs:
<https://github.com/univrsal/dvds3>. If you like it, that's who to thank.

This repo is a maintenance fork, nothing more. OBS 33 removed a library
function the old build relied on, so the plugin stopped loading, and upstream
hasn't been touched since 2022. All I've done is get it loading again and fix
the bugs that were in the way. Same source, same settings, same look. No new
features, on purpose. `CHANGELOG.md` lists every difference.

Also thanks to [Ayowel](https://github.com/Ayowel), whose upstream pull
request #3 is included here.

If upstream starts moving again, use upstream.

## What you need

*  **OBS Studio 33.0 or newer.** This build won't load in OBS 32 or older.
   (For those, upstream's 1.1 release is the one to use.)
*  That's it. It's one small DLL and a data folder. No .NET, no Qt, no
   runtimes, nothing else to install.

Upstream's own issue tracker lives at <https://git.vrsal.cc/alex/dvds3/issues>;
anything about this fork goes to the tracker at the bottom of this page.

## Install

Grab the file for your OS from [Releases](https://github.com/zglate/dvds3/releases).

### Windows

1.  Already have an older version? Remove it first - see
    [Upgrading from 1.x](#upgrading-from-1x).
2.  Unzip the download. You'll get a `dvd-screensaver` folder with
    `dvd-screensaver.dll`, a `data` folder and a `.pdb` file (debug symbols,
    harmless, helps if you ever send a crash report) inside it.
3.  Drop that whole folder into `C:\ProgramData\obs-studio\plugins\` so you
    end up with
    `C:\ProgramData\obs-studio\plugins\dvd-screensaver\dvd-screensaver.dll`.
    (`ProgramData` is hidden - paste the path into Explorer's address bar.
    Create `plugins` if it isn't there.)
4.  Start OBS. No source showing up? See
    [Turning the plugin back on](#turning-the-plugin-back-on).

### macOS

Open the `.pkg` and follow the prompts. It isn't signed with an Apple
Developer ID, so the first open gets blocked: open **System Settings →
Privacy & Security**, scroll down, click **Open Anyway**, then run the
`.pkg` again. It installs to
`~/Library/Application Support/obs-studio/plugins/`.

### Linux (Ubuntu 26.04 x86_64, OBS from the obsproject PPA)

```
sudo apt install ./dvd-screensaver-2.0.0-x86_64-linux-gnu.deb
```

That puts it in OBS 33's plugin folders under `/usr`. The `.deb` is built on
Ubuntu 26.04 against the PPA's OBS, so other Ubuntu/Debian versions and
other distros: build from source (below) or unpack the `.deb` by hand.

Heads up: if there's no `.deb` on the release yet, it's because the
obsproject PPA hadn't published OBS 33 for 26.04 when the release was cut
(CI refuses to build against anything older). It gets added as soon as the
PPA catches up. Building from source against your own OBS 33 works
meanwhile.

## Upgrading from 1.x

The old versions lived somewhere OBS 33 flags as **Legacy** and, per the
[OBS project](https://obsproject.com/kb/legacy-plugin-locations), OBS 34 will
stop loading. Remove the old copy before installing 2.0:

*  **Installed with the old `.exe`?** Settings → Apps → Installed apps (or
   Programs and Features), uninstall **dvd-screensaver**.
*  **Copied the files in by hand?** Delete
   `C:\Program Files\obs-studio\obs-plugins\64bit\dvd-screensaver.dll`
   (and the `.pdb` if it's there) plus the folder
   `C:\Program Files\obs-studio\data\obs-plugins\dvd-screensaver\`.
*  **Not sure which?** Tools → Plugin Manager → Installed. A row marked
   **Legacy** is a copy still sitting in an old spot.

Your scenes carry over as-is. The source type, its name and every setting are
unchanged. Which is also why you can't leave both installed: they register
the same source, and OBS will only use one of them.

One-time cleanup if you ever bounced another source: 1.x quietly stuck a
colour filter called `dvd-filter` onto that source and OBS saved it with your
scene collection. Right-click the source → Filters and remove `dvd-filter` if
it's listed. 2.0 never touches your other sources.

## Turning the plugin back on

When OBS 33 failed to load the old version it switched the plugin **off**, and
it stays off until you flip it back. So after installing 2.0 the first start
may still show nothing. Fix:

1.  Tools → Plugin Manager → Installed.
2.  Find **DVD screen saver**. If it shows a **Disabled** chip, flip the toggle on.
3.  OBS says *"OBS Studio must be restarted to apply plugin changes"*. Restart.

If the row says **Error** instead, the files are in the wrong place. Re-check
the install path. To confirm it loaded: Help → Log Files → View Current Log,
and `dvd-screensaver.dll` should be under *Loaded Modules*.

## Using it

Add Source → **DVD screen saver**.

| Setting | What it does |
|---|---|
| Source | *Image* (the file below) or any other video source in your collection |
| Image file | The logo to bounce. Defaults to the DVD logo |
| Width / Height | Size of the box it bounces around in |
| Speed | Pixels per second |
| Image scale | Scales the logo |
| Color shift | Shifts the hue 45° on every bounce (not on macOS) |
| Reset | Random position, random direction |

## Building from source

It's the official [OBS plugin template](https://github.com/obsproject/obs-plugintemplate),
so with CMake 3.28+ and the toolchain the template lists:

```
cmake --preset windows-x64        # or macos, ubuntu-x86_64
cmake --build --preset windows-x64
```

The first configure downloads the OBS 33 sources and dependencies pinned in
`buildspec.json`. CI builds and packages all three platforms on every tag.

## Issues

For this fork: <https://github.com/zglate/dvds3/issues>. Let me know if you
hit something.

## License

GPL-2.0-or-later, same as upstream. Original code copyright univrsal; the
changes in this fork copyright zglate, under the same license. See
`COPYING.txt`.
