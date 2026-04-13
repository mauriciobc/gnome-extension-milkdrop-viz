# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**gnome-extension-milkdrop-viz** is a MilkDrop-style audio visualizer for GNOME Shell 47/48/49 on **Wayland only**. It renders psychedelic visuals synchronized to system audio via libprojectM. The project is a **two-process system**: a C renderer binary + a GJS GNOME Shell extension.

## Build Commands

```bash
# Initial setup
meson setup build

# Compile
meson compile -C build

# Reconfigure after meson.build changes
meson setup --reconfigure build

# Install (extension + binary + schema)
meson install -C build

# Feature flags
meson setup build -Dprojectm=disabled    # without libprojectM
meson setup build -Dpipewire=disabled    # without PipeWire audio
```

**Required deps:** gtk4 >=4.10, glib-2.0 >=2.76, gio-2.0 >=2.76, epoxy >=1.5, egl
**Optional deps:** libprojectM-4, libprojectM-4-playlist, libpipewire-0.3, libspa-0.2

## Test Commands

```bash
# Run all tests
meson test -C build

# Run a single test by name
meson test -C build ring-buffer
meson test -C build control-protocol
meson test -C build render-pipeline
meson test -C build gtk-glarea-projectm   # requires DISPLAY
meson test -C build scaffold-validation   # Python extension validation

# Verbose output
meson test -C build <test-name> --print-errorlogs --verbose

# Integration test (requires installation)
meson setup --reconfigure build -Dshell-integration-tests=true
meson test -C build compositor-behavior-integration
```

Test names: `ring-buffer`, `backends`, `control-protocol`, `presets`, `audio-alignment`, `render-pipeline`, `gtk-glarea-projectm`, `gtk-glarea-fbo`, `tick-state-machine`, `control-state-flow`, `presets-edge-cases`, `scaffold-validation`

## Helper Scripts

```bash
tools/install.sh    # Install extension
tools/uninstall.sh  # Remove extension
tools/reload.sh     # Reload extension in running GNOME Shell
```

## Architecture

### Two-Process Model

```
milkdrop (C binary)          GNOME Shell Extension (GJS)
─────────────────────        ───────────────────────────
PipeWire audio capture  ←→   Process lifecycle (spawn/kill)
Lock-free ring buffer        GSettings routing
libprojectM rendering        Compositor scene graph anchoring
GTK4 GtkGLArea window        Window detection + reparenting
Unix socket control server
```

The extension spawns the binary, routes settings changes to it via the Unix socket, and anchors its window into the Clutter scene graph at the wallpaper layer (below all application windows).

### Thread Model (C binary)

| Thread | Role |
|--------|------|
| Main/GL | GTK main loop, GtkGLArea render signal, all projectM calls |
| PipeWire | Audio capture, ring buffer writes |
| Control | Unix socket accept/read/write |

**Strict invariants:**
- No OpenGL/projectM calls outside the GL thread
- No per-frame IPC in steady state (socket used only for user commands)
- No heavy rendering in the Shell process

### Per-Frame Data Flow

PipeWire thread → `ring_push()` (~50ns, lock-free) → GL thread → `ring_read()` → `projectm_pcm_add_float()` → `projectm_render_frame()` → OpenGL output

### Control Protocol

Text-based commands over Unix domain socket:
`status`, `opacity <0.0-1.0>`, `pause <on|off>`, `shuffle <on|off>`, `overlay <on|off>`, `preset-dir <path>`, `save-state`, `restore-state` (see `docs/research/09-control-socket-settings-and-state.md`)

Settings routed to socket at runtime: `opacity`, `preset-dir`, `shuffle`, `overlay`
Settings requiring restart: `monitor`

### Key Source Files

- `src/app.h` — `AppData` struct (shared state), inline lock-free ring buffer ops
- `src/main.c` — GTK4 app entry, `on_realize()` (GL/projectM init), `on_render()` (frame loop)
- `src/renderer.c` — `renderer_apply_resize()`, `renderer_frame_prep()` (PCM read + projectM feed)
- `src/audio.c` — PipeWire capture, `audio_align_stereo_float_count()`
- `src/control.c` — Unix socket server, command parsing, atomic flag updates
- `src/presets.c` — `.milk` file scanning, deterministic sort
- `extension/milkdrop@mauriciobc.github.io/extension.js` — Main extension class, all compositor anchoring
- `extension/milkdrop@mauriciobc.github.io/prefs.js` — Adwaita preferences UI

## C Code Style

- `#pragma once` for header guards
- Return type on its own line above function name; multiple parameters each on their own line
- `static` for all file-local functions
- Error handling: `g_warning()` for non-fatal, `goto fail` pattern for cleanup
- Allocators: `g_new0`/`g_strdup`/`g_free`; pair every allocation with a clear free path; `g_clear_pointer()`
- Zero-init structs: `= {0}`
- Shared state: `_Atomic` types with `atomic_load`/`atomic_store`; `GMutex` for non-atomic data
- Ring buffer: `memory_order_relaxed` for indices, `memory_order_acquire`/`memory_order_release` for sync
- Feature guards: `#if HAVE_PIPEWIRE` / `#if HAVE_PROJECTM`

## GJS Code Style

- GI imports first (`gi://Gio`), then Shell resources, then extension base class
- Single export: `export default class MilkdropExtension extends Extension`
- `camelCase` methods, `_` prefix for private, `CONSTANT_CASE` for module-level constants
- `enable()` and `disable()` must be perfectly symmetric — track all signal IDs and source IDs, null them in `disable()`
- All Shell API calls wrapped in try/catch; log with `log('[milkdrop] ...')` prefix
- No module-scope side effects; all init in `enable()`
- Use `Gio.SubprocessLauncher` for spawning, `Meta.WaylandClient` on Wayland
- Use `InjectionManager` for Shell overrides; call `clear()` in `disable()`

## Important Constraints

- **Wayland only** — no X11 support
- **libprojectM 4.x** — not 3.x; use the C API (not C++)
- **GTK4 GtkGLArea** for windowing (GNOME 48 dropped wlr-layer-shell-v1)
- **Dynamic linking only** — this is a system component
- **Do NOT modify `reference_codebases/`** — read-only reference material
- HiDPI: always multiply window size by `gtk_widget_get_scale_factor()`

## Documentation Map

- `PRD.md` — Product requirements, final decisions (canonical)
- `AGENTS.md` — Architecture overview, code style guide
- `docs/research/` — Deep-dive research documents (canonical references)
  - `02-system-architecture.md` — Two-process model details
  - `06-gtk4-glarea-projectm-integration.md` — GL integration, HiDPI
  - `08-pipewire-audio-ring-buffer-and-realtime.md` — Audio capture design
  - `09-control-socket-settings-and-state.md` — Control protocol design
- `.github/copilot-instructions.md` — Project guidelines
- `.github/skills/gnome-shell-extension-dev/` — GNOME extension rules
- `.cursor/skills/opengl/` — OpenGL integration guidelines
- `docs/legacy/` — Historical context (informational only)
