# Third-party reference snapshots

This folder holds **verbatim copies** of selected files from other projects, for **side-by-side comparison** with gnome-milkdrop (see [extension-benchmark.md](../extension-benchmark.md)). These files are **not** compiled or imported by the extension; they are documentation-only references.

## How these snapshots were produced

Shallow `git clone --depth 1` into `/tmp`, then `cp` of specific paths. Upstream commit SHAs (as of fetch):

| Directory | Upstream repository | Commit | License in folder |
|-----------|---------------------|--------|-------------------|
| `hanabi/` | [jeffshee/gnome-ext-hanabi](https://github.com/jeffshee/gnome-ext-hanabi) | `b02101014a34ba053edaa64e2ec142d0d2f0f6f9` | GPL-3.0 |
| `sound-visualizer/` | [raihan2000/visualizer](https://github.com/raihan2000/visualizer) | `e79f05a5b2eb7dea14f1c71ee39fb9e1a637ca10` | GPL-3.0 |
| `dynamic-music-pill/` | [Andbal23/dynamic-music-pill](https://github.com/Andbal23/dynamic-music-pill) | `9e66cb6767cf0bae0848888606f2a14ae3c85205` | GPL-3.0 |
| `live-lock-screen/` | [nick-redwill/LiveLockScreen](https://github.com/nick-redwill/LiveLockScreen) | `0a9fa4d8975bff16ffa727e9095371646ec10d34` | AGPL-3.0 |
| `live_wallpaper_gnome/` | [tridoxx/live_wallpaper_gnome](https://github.com/tridoxx/live_wallpaper_gnome) | `0e6baeb2ceb460eb8e6d46d97ee7d79c036a7bf2` | (no LICENSE file upstream; treat as all-rights-reserved until clarified) |
| `cava/` | [karlstav/cava](https://github.com/karlstav/cava) | `01bdbd52db7ac6cf11c0d488e68f30a4a97e547c` | MIT (see `cava/LICENSE`) |

**AGPL note:** `live-lock-screen` is AGPL-3.0. Do not merge its code into this project without a license compatibility review; use it only as a **read-only** reference for GStreamer/`gtk4paintablesink` patterns.

## What each snapshot is for

| Compare | With gnome-milkdrop |
|---------|---------------------|
| `hanabi/src/launcher.js` | `Meta.WaylandClient` / `new_subprocess` (Shell 49+) vs your renderer spawn path in [monitor.js](../../src/extension/monitor.js) |
| `hanabi/src/wallpaper.js` | `Clutter.Clone` of renderer vs [wallpaper.js](../../src/extension/wallpaper.js) |
| `hanabi/src/gnomeShellOverride.js` | Background actor injection vs your overrides |
| `hanabi/src/windowManager.js` | Hiding renderer from overview / window tracking |
| `hanabi/src/dbus.js`, `renderer/renderer.js` | D-Bus IPC vs your Unix-socket JSON ([ipc.js](../../src/extension/ipc.js)) |
| `hanabi/src/extension.js` | Extension bootstrap layout |
| `sound-visualizer/src/visual.js` | GStreamer-based spectrum / source handling vs [audio.js](../../src/extension/audio.js) |
| `dynamic-music-pill/src/visualizerEngine.js`, `uiVisualizers.js` | Subprocess + **cava** vs in-process GStreamer |
| `live-lock-screen/external/pipeline.js` | Declarative GStreamer string → `gtk4paintablesink` loop |
| `live_wallpaper_gnome/extension.js` | Small Shell extension using **Clutter.Video** on the desktop |
| `cava/input/pipewire.c`, `pulse.c`, `common.c` | Native PipeWire/Pulse capture (contrast with `pipewiresrc`/`pulsesrc` in GStreamer) |

## Not vendored here

- **Clapper**, **projectM**, **GStreamer** upstream: too large; use clones or distro source packages when needed.
- **Dynamic Music Pill** `controller.js` / large UI files: omitted; only visualization-related modules are included.

## Refreshing snapshots

```bash
BASE=/tmp/milkdrop-ref-$$
git clone --depth 1 https://github.com/jeffshee/gnome-ext-hanabi.git "$BASE/hanabi"
# … then copy files and update the table above with `git rev-parse HEAD`.
```
