# gnome-extension-milkdrop-viz

MilkDrop-style audio visualizer for GNOME Shell on Wayland, built as a two-process system:

- Native C renderer (`milkdrop`) for audio capture, ring buffer, and OpenGL/projectM rendering
- GNOME Shell extension (GJS) for lifecycle supervision, settings routing, and compositor anchoring

Targeted GNOME Shell versions: 47, 48, 49, 50 (Wayland only).

## Status

This project is actively developed. Core renderer, control protocol, extension scaffold, and tests are present.

## Why Two Processes?

GNOME Shell extensions run inside the shell process, so heavy rendering must stay out of-process for stability and performance.

- C renderer process owns:
  - PipeWire capture (optional)
  - lock-free audio ring buffer
  - projectM frame rendering
  - Unix control socket
- GJS extension owns:
  - enable/disable lifecycle
  - subprocess supervision and restart policy
  - GSettings bindings and runtime settings forwarding
  - compositor integration and window anchoring

Architecture invariants:

- No OpenGL/projectM calls outside the render thread
- No per-frame IPC in steady state
- No heavy rendering work in the GNOME Shell process

## Repository Layout

```text
src/                          C renderer (GTK4 + GLArea + control/audio modules)
extension/milkdrop@.../       GNOME Shell extension and prefs UI
data/                         GSettings schema
tests/                        C unit tests + scaffold/integration validation
docs/research/                Design and architecture deep dives
tools/                        install/uninstall/reload helper scripts
```

## Dependencies

Required (base build):

- `glib-2.0 >= 2.76`
- `gio-2.0 >= 2.76`
- `gtk4 >= 4.10`
- `epoxy >= 1.5`
- EGL/OpenGL runtime

Optional:

- `libprojectM-4` and `libprojectM-4-playlist`
- `libpipewire-0.3` and `libspa-0.2`

## Build

```bash
meson setup build
meson compile -C build
```

Reconfigure after Meson option changes:

```bash
meson setup --reconfigure build
```

Feature flags:

```bash
meson setup build -Dprojectm=disabled
meson setup build -Dpipewire=disabled
```

## Install

```bash
meson install -C build
```

Helper scripts:

```bash
./tools/install.sh
./tools/uninstall.sh
./tools/reload.sh
```

Contributor / diagnostics scripts (see script headers for usage):

```bash
./tools/nested_devkit.sh
./tools/profile_gpu_milkdrop.sh
./tools/debug_nested.sh
python3 ./tools/analyze_on_render_journal.py
```

## Test

Run all registered tests:

```bash
meson test -C build
```

Run one test:

```bash
meson test -C build ring-buffer --print-errorlogs --verbose
```

Optional nested GNOME Shell integration test:

```bash
meson setup --reconfigure build -Dshell-integration-tests=true
meson test -C build compositor-behavior-integration
```

Isolated nested Shell + extension (manual UI test):

```bash
./tools/nested_devkit.sh
```

Run that **from a terminal inside your GNOME session** (e.g. Console or Terminal on the same machine). The mutter-devkit window embeds in the **current** compositor; if `WAYLAND_DISPLAY` / `DISPLAY` are unset (plain SSH, CI, some IDE terminals), **no window appears**. Use Overview / Alt+Tab if it opened behind other windows.

## Runtime Control Protocol

The renderer exposes a Unix domain control socket with line commands:

- `status`
- `opacity <0.0-1.0>`
- `pause <on|off>`
- `shuffle <on|off>`
- `overlay <on|off>`
- `preset-dir <absolute-path>`
- `next`
- `previous`
- `load-preset <absolute-path>` — load a single `.milk` file
- `save-state` — returns a one-line snapshot (see docs)
- `restore-state [key=value ...]` — apply snapshot fields

Canonical format, buffer limits, and extension notes: [`docs/research/09-control-socket-settings-and-state.md`](docs/research/09-control-socket-settings-and-state.md).

## GSettings

Schema ID: `org.gnome.shell.extensions.milkdrop`

Keys:

- `enabled` (bool)
- `monitor` (int)
- `opacity` (double 0.0 to 1.0)
- `preset-dir` (string)
- `shuffle` (bool)
- `overlay` (bool)
- `preset-rotation-interval`, `pause-on-fullscreen`, `pause-on-maximized`, `media-aware`, `fps`, `all-monitors`, `last-preset`, `was-paused`, `use-discrete-gpu`, `respect-reduced-motion` (see `data/org.gnome.shell.extensions.milkdrop.gschema.xml`)

## Wayland scope

The extension and renderer are intended for **Wayland** sessions only; the renderer is spawned via `Meta.WaylandClient`.

## Documentation

- Product requirements: `PRD.md`
- Canonical technical research: `docs/research/`
- Historical notes: `docs/legacy/`

## License

This project is licensed under the MIT License. See `LICENSE`.