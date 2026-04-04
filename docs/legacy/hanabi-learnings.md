# Hanabi Learnings

This document records what gnome-milkdrop inherits from Hanabi and where it intentionally diverges.

## Confirmed Useful Patterns

### Separate renderer process

Hanabi runs a standalone GTK4 renderer script and launches it from the extension. That pattern is relevant because it keeps heavy rendering and renderer crashes outside the gnome-shell process.

### Wayland subprocess ownership

Hanabi uses Meta.WaylandClient ownership tracking for its renderer windows. This is especially relevant on GNOME 49, where Meta.WaylandClient subprocess creation moved to new_subprocess().

### Window tracking and hiding

Hanabi identifies renderer windows, tracks them through the shell, and excludes them from overview- and app-facing flows. That validates the general class of solution, but milkdrop should reuse the minimum surface needed instead of copying all shell overrides upfront.

### Standalone renderer debug path

Hanabi exposes a renderer-only run path in its helper script. gnome-milkdrop should keep the same property because renderer-only debugging is essential before shell integration is stable.

## Planned Divergences

### Rendering model

Hanabi renders shared paintables or media playback. gnome-milkdrop renders procedural OpenGL output through GtkGLArea.

### IPC

Hanabi uses D-Bus for some renderer control. gnome-milkdrop will start with a small Unix-socket JSON protocol because it needs frequent frame-state delivery.

### Process layout

Hanabi behaves more like a global wallpaper renderer. gnome-milkdrop will manage one renderer per monitor once monitor orchestration is implemented.

### Evaluation path

Hanabi does not have MilkDrop-style expression evaluation. gnome-milkdrop needs both per-frame and per-vertex evaluation boundaries.

## Items To Revalidate During Implementation

- Exact z-order behavior on GNOME 49 in real sessions
- Whether title tagging is still the least fragile renderer-identification strategy
- Whether overview exclusion requires shell overrides or can rely on window flags alone
- Whether GtkGraphicsOffload is available and beneficial on the target GTK and driver stack
