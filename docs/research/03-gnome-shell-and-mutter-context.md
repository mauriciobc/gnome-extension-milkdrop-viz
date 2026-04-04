# GNOME Shell and Mutter Context

## Platform model

GNOME Shell is the desktop shell layer running on top of Mutter, GNOME’s window manager and compositor. Extension code runs in the Shell’s JavaScript environment rather than as an isolated application process, which means extension mistakes have system-level impact compared with normal app bugs.[cite:27][cite:30][cite:18]

This matters for gnome-milkdrop because the project adds desktop integration but should not become part of the rendering or audio hot path.

## Consequences for architecture

The Shell extension must be treated as an in-process plugin with strict discipline. That means no experimental rendering, no long-lived busy loops, no compute-heavy logic, no blocking I/O on the main path, and no toolkit mixing that violates extension guidance.[cite:16][cite:18]

The renderer process can be ambitious in C/GTK/OpenGL because its failures are isolated. The extension cannot.

## Extension runtime characteristics

GNOME Shell extensions use GJS and Shell libraries such as `St`, `Shell`, and `Meta` when running in the Shell environment. Preferences code is separate and uses GTK/Adwaita-side APIs rather than Shell-side UI classes.[cite:16][cite:18]

A reference implementation for this project should preserve a strict distinction between:

- **Shell runtime code**: panel entry, quick settings integration, process monitoring, setting listeners.
- **Preferences code**: configuration UI, validation hints, explanatory copy.

## Versioning reality

GNOME Shell extension compatibility is version-sensitive. Review guidance warns against claiming support for future versions that have not been tested, and metadata should list only supported Shell versions.[cite:16]

Therefore, development must maintain an explicit tested-version matrix. Do not mark support optimistically.

## Why not render in Shell

Nothing in the GNOME guidance encourages putting an embedded OpenGL visualizer into the Shell process. Even before performance concerns, this would complicate lifecycle, increase failure blast radius, and create compatibility problems with Shell internals.

For this project, the correct interpretation is that Shell integration should expose controls and state, while the visualization remains a standalone GTK-rendered surface.

## Shell-safe design principles

- Keep imports minimal.
- Avoid side effects at module load time.[cite:16]
- Allocate only what is needed in `enable()` and release everything in `disable()`.[cite:16]
- Use clear recovery behavior when the subprocess disappears.
- Treat user settings changes as idempotent events.

## Implication for future features

If future versions add overlays, richer Shell UI, or media-session coupling, those features must preserve the existing boundary: the Shell controls the renderer but does not become the renderer.
