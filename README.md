# gnome-extension-milkdrop-viz

MilkDrop-style audio visualizer for GNOME Shell on Wayland.

Current project version: `0.2.0-alpha.1`

This is a prerelease build line. The renderer, extension lifecycle, settings flow, and test suite are in active use, but the project is not yet broadly tested across GNOME, Mesa, NVIDIA, and multi-monitor combinations.

## Current Architecture

The project is a two-process system:

- Native C renderer (`milkdrop`) for audio capture, ring buffer management, projectM rendering, screenshot/state helpers, and the local control socket.
- GNOME Shell extension (GJS) for process supervision, monitor-aware spawning, settings routing, pause policy, MPRIS integration, and compositor anchoring.

The production renderer uses an SDL2 hidden OpenGL context and renders projectM into an offscreen FBO. Frames are read back with `glReadPixels()` and published into a GTK4 `GtkPicture` as a `GdkMemoryTexture`.

Supported GNOME Shell versions in metadata: 47, 48, 49, 50.

Core invariants:

- No OpenGL or projectM calls outside the renderer GL thread.
- No per-frame IPC in steady state.
- No heavy rendering work inside the GNOME Shell process.
- Control-thread requests cross into rendering only through atomics or locked handoff state.

## Repository Layout

```text
src/                          C renderer, control server, audio path, offscreen renderer
extension/milkdrop@.../       GNOME Shell extension, prefs UI, pause policy, MPRIS watcher
data/                         GSettings schema
tests/                        C, GJS, and smoke-style validation targets
docs/research/                Current architecture and design references
docs/legacy/                  Historical notes and discarded approaches
tools/                        install, reload, uninstall, and evidence collection helpers
```

## Dependencies

Required base dependencies:

- `glib-2.0 >= 2.76`
- `gio-2.0 >= 2.76`
- `gtk4 >= 4.10`
- `epoxy >= 1.5`
- EGL/OpenGL runtime

Feature-dependent dependencies:

- `sdl2` when building with projectM enabled
- `libprojectM-4` and `libprojectM-4-playlist` for visual rendering
- `libpipewire-0.3` and `libspa-0.2` for live system-audio capture

## Build

```bash
meson setup build
meson compile -C build
```

Reconfigure after Meson option changes:

```bash
meson setup --reconfigure build
```

Useful feature flags:

```bash
meson setup build -Dprojectm=disabled
meson setup build -Dpipewire=disabled
meson setup build -Dshell-integration-tests=true
```

## Install And Reload

```bash
meson install -C build
./tools/install.sh
./tools/uninstall.sh
./tools/reload.sh
```

## Test

Run the full registered test suite:

```bash
meson test -C build
```

Focused examples:

```bash
meson test -C build control-protocol --print-errorlogs --verbose
meson test -C build offscreen-renderer --print-errorlogs --verbose
meson test -C build ring-buffer --print-errorlogs --verbose
meson test -C build pause-policy --print-errorlogs --verbose
```

Targets that rely on a display or local install state are already isolated in Meson. The GL-sensitive tests `gtk-glarea-projectm`, `gdk-glcontext-projectm`, and `offscreen-renderer` run with `is_parallel: false` because some drivers leave GL state behind.

## Runtime Control Protocol

The renderer exposes a per-monitor Unix socket at `$XDG_RUNTIME_DIR/milkdrop-<monitor>.sock` by default. The protocol is line-delimited text, not binary.

Current commands:

- `status`
- `opacity <0.0-1.0>`
- `pause <on|off>`
- `shuffle <on|off>`
- `overlay <on|off>`
- `preset-dir <absolute-path>`
- `next`
- `previous`
- `fps <10-144>`
- `rotation-interval <5-300>`
- `beat-sensitivity <0.0-5.0>`
- `hard-cut-enabled <on|off>`
- `hard-cut-sensitivity <0.0-5.0>`
- `hard-cut-duration <1.0-120.0>`
- `soft-cut-duration <1.0-30.0>`
- `save-state`
- `restore-state [preset-path] [0|1]`
- `screenshot <absolute-path>`

`status` currently returns newline-delimited `key=value` fields, including `paused`, `opacity`, `shuffle`, `overlay`, `quarantine`, `audio`, `fps`, and `preset`.

## Settings Surface

Schema ID: `org.gnome.shell.extensions.milkdrop`

Current keys are:

- `enabled`, `monitor`, `all-monitors`
- `opacity`, `overlay`, `fps`, `gpu-profile`
- `preset-dir`, `shuffle`, `preset-rotation-interval`
- `beat-sensitivity`, `hard-cut-enabled`, `hard-cut-sensitivity`, `hard-cut-duration`, `soft-cut-duration`
- `pause-on-fullscreen`, `pause-on-maximized`, `pause-on-empty-desktop`, `media-aware`, `stop-renderer-when-idle`
- `last-preset`, `was-paused`

The preferences window includes live status polling, GPU profile selection, rendering controls, pause-policy controls, and transition tuning.

## Runtime Evidence Collection

When debugging session failures, collect a bounded evidence bundle before changing code:

```bash
./tools/collect_issue_evidence.sh --since "30 minutes ago" --core-limit 5
```

The script captures environment, GSettings, user journal excerpts, and the newest matching coredumps under `logs/`.

## Documentation Map

- `AGENTS.md`: concise implementation quick-start and current architecture notes
- `docs/research/`: current technical references
- `PRD.md`: original design document, now historical where it conflicts with the shipped SDL2 offscreen architecture
- `docs/legacy/`: exploratory and superseded material

## Scope

Wayland only. X11 support is out of scope.

## License

MIT. See `LICENSE`.