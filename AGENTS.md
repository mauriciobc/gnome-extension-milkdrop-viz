# AGENTS.md - gnome-extension-milkdrop-viz

## Purpose

This file is the canonical quick-start for AI coding agents in this repository.
Keep it short, actionable, and linked to canonical docs instead of duplicating them.

## Core Architecture

Two-process model (must be preserved):
- C renderer in [src](src): PipeWire capture, lock-free ring buffer, SDL2 offscreen + projectM rendering, Unix control socket (line-based JSON protocol).
- GJS extension in [extension/milkdrop@mauriciobc.github.io](extension/milkdrop@mauriciobc.github.io): GNOME Shell lifecycle, background injection via `Meta.WaylandClient`, process supervision per monitor, settings routing, compositor anchoring.

Target GNOME Shell: 47, 48, 49, 50. Wayland only.

Non-negotiable invariants:
- No OpenGL or projectM calls outside the renderer GL thread.
- No per-frame IPC in steady state.
- No heavy rendering work in GNOME Shell process.
- Control thread communicates with GL thread only via atomic flags.

## Architecture Evolution Notes

The codebase has evolved beyond the original PRD spec:
- **SDL2 offscreen renderer**: projectM now renders to an SDL2 GL context, then blits to GtkPicture via CPU readback. See [docs/research/14-renderer-sdl2-offscreen-architecture.md](docs/research/14-renderer-sdl2-offscreen-architecture.md) and [src/offscreen_renderer.c](src/offscreen_renderer.c).
- **Line-based control protocol**: The socket uses line-delimited JSON commands (not the binary protocol from the PRD). See [src/control.c](src/control.c).
- **Background injection**: The extension uses `Meta.WaylandClient` and background actor injection for wallpaper anchoring, not simple Clutter reparenting. See [extension/milkdrop@mauriciobc.github.io/extension.js](extension/milkdrop@mauriciobc.github.io/extension.js).
- **Multi-monitor**: `all-monitors` GSettings key spawns one renderer instance per monitor.
- **GPU profile selection**: `gpu-profile` GSettings key injects PRIME offloading environment variables at renderer spawn time (`DRI_PRIME=1` for Mesa, `__NV_PRIME_RENDER_OFFLOAD=1` for NVIDIA Optimus). `MESA_GL_VERSION_OVERRIDE` is suppressed when the NVIDIA profile is active.
- **Modular extension**: Split into `controlClient.js`, `managedWindow.js`, `pausePolicy.js`, `mprisWatcher.js`, `constants.js`.

## Build And Test

Primary commands:

```bash
meson setup build
meson compile -C build
meson test -C build
```

Reconfigure after Meson option changes:

```bash
meson setup --reconfigure build
```

Useful configuration flags:

```bash
meson setup build -Dprojectm=disabled
meson setup build -Dpipewire=disabled
meson setup build -Dshell-integration-tests=true
```

Focused test runs:

```bash
meson test -C build control-protocol --print-errorlogs --verbose
meson test -C build offscreen-renderer --print-errorlogs --verbose
meson test -C build ring-buffer --print-errorlogs --verbose
```

Environment notes:
- Display-dependent tests require DISPLAY.
- Tests touching control sockets should use MILKDROP_TEST_ISOLATE_SOCKET=1.
- Shell integration test requires -Dshell-integration-tests=true and local install state.
- `gtk-glarea-projectm`, `gdk-glcontext-projectm`, and `offscreen-renderer` run with `is_parallel: false` — some drivers leave GL state that breaks subsequent test readback.

## Install And Debug

```bash
meson install -C build
./tools/install.sh        # build + install to ~/.local
./tools/uninstall.sh
./tools/reload.sh         # disable + enable extension
```

Collect runtime evidence when debugging in-session failures:

```bash
./tools/collect_issue_evidence.sh --since "30 minutes ago" --core-limit 5
```

## Critical Renderer Rules

When touching projectM or GL code in [src/main.c](src/main.c) and [src/offscreen_renderer.c](src/offscreen_renderer.c):
- Call glFinish immediately after projectm_opengl_render_frame_fbo to synchronize multi-pass blur.
- Restore GL state before returning from the frame wind-down path (program, texture, active texture, blend/depth/stencil/scissor).
- Keep projectM lifecycle calls on the SDL2 GL context only (make_current in init, begin_frame at render time, shutdown cleanup).

See canonical details in [docs/research/13-projectm-integration-compliance.md](docs/research/13-projectm-integration-compliance.md).

## GSettings

Schema: `org.gnome.shell.extensions.milkdrop`

Keys beyond the obvious: `preset-rotation-interval`, `fps`, `beat-sensitivity`, `hard-cut-enabled`, `hard-cut-sensitivity`, `hard-cut-duration`, `soft-cut-duration`, `all-monitors`, `pause-on-fullscreen`, `pause-on-maximized`, `media-aware`, `last-preset`, `was-paused`.

Full schema in [data/org.gnome.shell.extensions.milkdrop.gschema.xml](data/org.gnome.shell.extensions.milkdrop.gschema.xml).

## Conventions

- Treat [reference_codebases](reference_codebases) as read-only.
- Keep patches small and reviewable.
- Preserve enable/disable symmetry in GJS extension lifecycle.
- Avoid module-scope side effects in extension code.
- Do not create `src/gl/`, `src/fft.c`, `src/expr.cpp`, or vendor directories — libprojectM replaces all of that.
- Stale build dirs: `bundle_projectm_build_dir` is a deprecated meson option kept only so old build trees can reconfigure without errors.

## Documentation Map

Start here, link instead of copying details:
- Product and constraints: [PRD.md](PRD.md)
- Research index: [docs/research](docs/research)
- System architecture: [docs/research/02-system-architecture.md](docs/research/02-system-architecture.md)
- GNOME Shell constraints: [docs/research/03-gnome-shell-and-mutter-context.md](docs/research/03-gnome-shell-and-mutter-context.md)
- Extension lifecycle guidance: [docs/research/04-gnome-shell-extension-development.md](docs/research/04-gnome-shell-extension-development.md)
- GTK4 GLArea and HiDPI: [docs/research/06-gtk4-glarea-projectm-integration.md](docs/research/06-gtk4-glarea-projectm-integration.md)
- OpenGL/GLSL principles: [docs/research/07-opengl-glsl-and-rendering-principles.md](docs/research/07-opengl-glsl-and-rendering-principles.md)
- PipeWire and ring buffer: [docs/research/08-pipewire-audio-ring-buffer-and-realtime.md](docs/research/08-pipewire-audio-ring-buffer-and-realtime.md)
- Control protocol and runtime state: [docs/research/09-control-socket-settings-and-state.md](docs/research/09-control-socket-settings-and-state.md)
- Testing and observability: [docs/research/11-testing-observability-and-performance.md](docs/research/11-testing-observability-and-performance.md)
- SDL2 offscreen architecture: [docs/research/14-renderer-sdl2-offscreen-architecture.md](docs/research/14-renderer-sdl2-offscreen-architecture.md)
- projectM compliance: [docs/research/13-projectm-integration-compliance.md](docs/research/13-projectm-integration-compliance.md)
- EGL image / dmabuf / cross-context texture sharing: [docs/research/15-egl-dmabuf-cross-context-texture-sharing.md](docs/research/15-egl-dmabuf-cross-context-texture-sharing.md)
- Meta.WaylandClient lifecycle and background injection: [docs/research/16-meta-wayland-client-lifecycle.md](docs/research/16-meta-wayland-client-lifecycle.md)
- GNOME Shell 47–50 extension API changes: [docs/research/17-gnome-shell-47-50-extension-api-changes.md](docs/research/17-gnome-shell-47-50-extension-api-changes.md)
- GNOME extension skill: [.github/skills/gnome-shell-extension-dev/SKILL.md](.github/skills/gnome-shell-extension-dev/SKILL.md)

## GPU Profile Selection

The `gpu-profile` GSettings key controls which GPU renders the visualizer on hybrid-graphics systems. It is applied at renderer spawn time via `Gio.SubprocessLauncher.setenv()`.

| Profile | Env vars injected |
|---|---|
| `default` | None (system default) |
| `dri-prime` | `DRI_PRIME=1` |
| `nvidia-optimus` | `__NV_PRIME_RENDER_OFFLOAD=1`, `__GLX_VENDOR_LIBRARY_NAME=nvidia`, `__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json` |

**Rules:**
- `MESA_GL_VERSION_OVERRIDE` is **not** set when `gpu-profile` is `nvidia-optimus` (NVIDIA proprietary does not use Mesa).
- Changing the profile triggers an immediate renderer restart (`_restartProcess()` for all active monitors).
- The renderer logs `GL_VENDOR`, `GL_RENDERER`, and `GL_VERSION` on startup when verbose mode is enabled so users can verify which GPU is active.

**Sources:**
- Mesa `DRI_PRIME`: https://docs.mesa3d.org/envvars.html#envvar-DRI_PRIME
- NVIDIA PRIME Render Offload: https://download.nvidia.com/XFree86/Linux-x86_64/565.77/README/primerenderoffload.html
- Arch Wiki PRIME (Wayland delay workaround): https://wiki.archlinux.org/title/PRIME

## Known Issues Fixed

### Startup preset activation deferred until first render cycle
Mesa's Gallium shader compiler dereferences NULL when `projectm_load_preset_file` is invoked before the first render cycle. This was caused by premature preset loading during startup. Fixed by deferring any preset load until `render_frame_counter > 0`, ensuring at least one `projectm_opengl_render_frame_fbo` call completes before the playlist is touched. The offscreen renderer also disables `GL_KHR_parallel_shader_compile` at init time to eliminate background compiler thread races.

### Missing canonical window type setup (Shell 49+)
The extension never called `window.hide_from_window_list()` or `window.set_type(Meta.WindowType.DESKTOP)`, causing Mutter to treat the renderer as a normal toplevel window. This produced "Buggy client" warnings and configure event races that destabilized the GL context. Fixed by adding canonical Shell 49+ window setup in `_anchorWindow()`. InjectionManager overrides are retained as fallback for Shell ≤48.

### Non-canonical MetaContext reference
Spawn code used `global.context` instead of the documented `global.display.get_context()`. Fixed to use the canonical API.

### Insufficient GL sync before preset load
`glFlush()` before `projectm_load_preset_file()` only submitted commands to the driver queue without waiting for completion. Mesa Gallium could still be in a partial FBO state when projectM started shader compilation. Fixed by replacing `glFlush()` with `glFinish()` to block until the FBO is fully ready. Performance impact is negligible — preset switches occur every 8-30 seconds, not per-frame.

## Repository Layout

```
milkdrop/
├── meson.build
├── meson_options.txt
├── src/
│   ├── app.h
│   ├── main.c
│   ├── audio.c/.h
│   ├── control.c/.h
│   ├── offscreen_renderer.c/.h
│   ├── renderer.c/.h
│   └── quarantine.c/.h
├── extension/milkdrop@mauriciobc.github.io/
│   ├── metadata.json
│   ├── extension.js
│   ├── prefs.js
│   └── controlClient.js, managedWindow.js, pausePolicy.js, mprisWatcher.js, constants.js
├── data/org.gnome.shell.extensions.milkdrop.gschema.xml
└── tools/
    ├── install.sh, uninstall.sh, reload.sh
    ├── collect_issue_evidence.sh
    └── run_controlled_projectm_fbo_test.sh
```