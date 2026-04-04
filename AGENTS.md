# AGENTS.md — gnome-extension-milkdrop-viz

## Architecture

Two-process model:
- **C renderer** (`src/`): PipeWire audio capture, ring buffer, projectM rendering, control socket
- **GJS extension** (`extension/`): GNOME Shell lifecycle, process supervision, settings routing, compositor anchoring

Strict boundaries: no OpenGL/projectM calls outside the render thread; no per-frame IPC in steady state; no heavy rendering in the Shell process.

## Project Structure

```
src/                          # C renderer (GTK4 + GLArea + libprojectM)
  app.h                       # Shared types: AppData, AudioRing, inline ring buffer ops
  main.c                      # GTK app lifecycle, GLArea callbacks, window management
  audio.c                     # PipeWire audio capture (optional, feature-flagged)
  control.c                   # Unix socket control protocol (opacity, shuffle, preset-dir)
  presets.c                   # Preset directory scanning and cycling
extension/milkdrop@mauriciobc.github.io/
  extension.js                # GNOME Shell extension (imports local ESM modules)
  constants.js                # Retry backoff constants (shared)
  controlClient.js            # Unix control socket client helpers
  prefs.js                    # Adw preferences UI
  metadata.json               # Extension metadata (shell-version, uuid)
data/                         # GSettings schema XML
tests/                        # C unit tests, Python scaffold validation, bash integration tests
reference_codebases/          # Read-only reference material (cava, hanabi, projectM vendor)
tools/                        # install.sh, uninstall.sh, reload.sh
docs/research/                # Canonical deep-dive documents for architecture decisions
```

## Build Commands

```bash
# Initial setup
meson setup build

# Compile
meson compile -C build

# Reconfigure (after meson.build changes)
meson setup --reconfigure build

# Install
meson install -C build

# Optional feature flags
meson setup build -Dprojectm=disabled   # build without libprojectM
meson setup build -Dpipewire=disabled   # build without PipeWire
meson setup build -Dshell-integration-tests=true   # register nested GNOME Shell test (see below)
```

## Test Commands

```bash
# Run all tests (default: no nested Shell integration test)
meson test -C build

# Enable compositor integration test when configuring the build (requires extension +
# renderer installed under ~/.local, see tests/test_compositor_behavior.sh)
meson setup --reconfigure build -Dshell-integration-tests=true
meson test -C build compositor-behavior-integration

# Run compositor checks without spawning nested gnome-shell (static assertions only)
./tests/test_compositor_behavior.sh --static-only

# Run a single test
meson test -C build ring-buffer
meson test -C build backends
meson test -C build control-protocol
meson test -C build presets
meson test -C build scaffold-validation

# Run a single test with verbose output
meson test -C build ring-buffer --print-errorlogs --verbose
```

Test files: `tests/test_ring_buffer.c`, `tests/test_backends.c`, `tests/test_control_protocol.c`, `tests/test_presets.c`, `tests/validate_scaffold.py`, `tests/test_compositor_behavior.sh`

C tests use GLib test framework (`g_test_init`, `g_test_add_func`, `g_assert_*` macros). Python tests validate extension scaffold integrity (metadata, schema, scripts). Bash integration tests are only registered when `-Dshell-integration-tests=true`.

## Renderer debugging

The `milkdrop` binary logs sparingly by default. Use **`milkdrop --verbose`** for GL init, resize, and periodic tick diagnostics. Log domain is **`milkdrop`** (`G_MESSAGES_DEBUG` applies to `g_debug` if used).

## Tools Scripts

```bash
tools/install.sh     # Install the extension
tools/uninstall.sh   # Remove the extension
tools/reload.sh      # Restart GNOME Shell extension
```

## Test Details

- **C unit tests** (`tests/test_*.c`): Use GLib test framework (`g_test_init`, `g_test_add_func`, `g_assert_*` macros). Each test file is compiled as a standalone executable linked against GLib/GTK.
- **Python scaffold validation** (`tests/validate_scaffold.py`): Checks metadata.json UUID/shell-version, GSettings schema keys/types/ranges, extension class declaration, prefs.js structure, and tool script executability/shebangs.
- **Bash integration tests** (`tests/test_compositor_behavior.sh`): Validates compositor-level integration behavior (run with `--integration` flag, 180s timeout).

## Data & Schema

- GSettings schema: `data/org.gnome.shell.extensions.milkdrop.gschema.xml`
- Schema ID: `org.gnome.shell.extensions.milkdrop`
- Keys: `enabled` (b), `monitor` (i), `opacity` (d, range 0.0–1.0), `preset-dir` (s), `shuffle` (b), `overlay` (b)

## C Code Style

- **Standard**: C11 (`-D_GNU_SOURCE`, `-std=c11`, warning level 2)
- **Headers**: `#pragma once` for header guards; one `.h` per `.c` module; shared types in `app.h`
- **Includes**: project headers first (`#include "module.h"`), then system/GLib/GTK headers, then conditional feature headers (`#if HAVE_PROJECTM`)
- **Naming**: `snake_case` for functions/variables; `PascalCase` for types/structs; `UPPER_SNAKE_CASE` for macros/constants
- **Functions**: return type on separate line from function name; each parameter on its own line when multiple; `static` for file-local functions
- **Error handling**: return `bool`/`gboolean` for success/failure; use `g_warning()` for non-fatal errors; use `goto fail` pattern for cleanup in init functions; log with `g_message()` for informational events
- **Null guards**: early return on NULL input; use `g_clear_pointer()` / `g_free()` for cleanup
- **Feature flags**: wrap optional deps in `#if HAVE_PIPEWIRE` / `#if HAVE_PROJECTM` blocks; provide `(void)var;` stubs for unused params when features are disabled
- **Threading**: use `_Atomic` types and `atomic_load`/`atomic_store` for shared state; `GMutex` for non-atomic shared data; `_Atomic` fields in `AppData` for cross-thread flags
- **Memory**: prefer GLib allocators (`g_new0`, `g_strdup`, `g_free`); pair every alloc with a clear free path; zero-init structs with `{0}`
- **Ring buffer**: lock-free single-producer/single-consumer; use `memory_order_relaxed` for index loads, `memory_order_acquire`/`memory_order_release` for synchronization

## GJS Code Style

- **Imports**: GI imports first (`gi://Gio`), then shell resources (`resource:///org/gnome/shell/...`), then extension base class
- **Class**: single `export default class MilkdropExtension extends Extension`
- **Naming**: `camelCase` for methods/properties; `_` prefix for private members; `CONSTANT_CASE` for module-level constants
- **Lifecycle**: `enable()` and `disable()` must be perfectly symmetric; track all signal IDs and source IDs; null out references in `disable()`
- **Error handling**: wrap compositor/Shell API calls in `try/catch`; log with `log('[milkdrop] ...')` prefix
- **No module-scope side effects**: all initialization happens in `enable()`
- **InjectionManager**: use for Shell prototype overrides; always call `clear()` in `disable()`
- **Process management**: spawn renderer via `Gio.SubprocessLauncher`; use `Meta.WaylandClient` on Wayland; implement exponential backoff retry with `GLib.timeout_add`
- **Compositor overrides**: hide renderer from overview, alt-tab, dash, and window lists; inject clone into background actors (Hanabi pattern)

## General Conventions

- Treat `reference_codebases/` as read-only; do not modify unless explicitly asked
- Keep patches small and reviewable; preserve documented architecture invariants
- For GNOME extension lifecycle, follow GNOME Shell extension development best practices
- Consult `PRD.md` for product constraints and `docs/research/` for deep technical dives
- Copilot instructions (`.github/copilot-instructions.md`): prefer small reviewable patches; preserve two-process architecture boundaries; consult research docs for design decisions; validate assumptions against docs before introducing build scripts
