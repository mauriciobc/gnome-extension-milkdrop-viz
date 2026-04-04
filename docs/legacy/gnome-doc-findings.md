# GNOME Documentation Findings

This project targets GNOME Shell 47, 48, and 49 and follows the current GJS extension model.

## Confirmed Rules From GNOME Documentation

### Extension shape

- Every extension needs metadata.json and extension.js.
- prefs.js is optional, but required once the project exposes a settings UI.
- ESModules are the supported model for GNOME 45 and later.

### Lifecycle discipline

- The extension constructor should only prepare static resources.
- Shell objects, signals, and GLib sources belong in enable().
- Everything created or connected in enable() must be cleaned up in disable().

### Process separation

- extension.js runs inside gnome-shell and must stay conservative because fatal mistakes affect the desktop session.
- prefs.js runs in a separate GTK process and is the safe place for GTK4 and Adwaita code.
- The shell process must not import Gtk, Gdk, or Adw.
- The prefs process must not import Meta, Shell, St, or Clutter.

### Review implications

- Companion processes are possible but review-sensitive.
- External scripts and binaries are discouraged, so the local-first architecture should keep these parts isolated and well documented.
- Logging volume must stay low.

### Compatibility notes

- GNOME 47 awaits prefs window builders.
- GNOME 49 requires the devkit command path for nested-shell testing.
- GNOME 49 changed Meta.WaylandClient subprocess handling and Meta.Window maximize APIs.

## Project Decisions Based On The Docs

- Use the UUID milkdrop@mauriciobc.github.io.
- Keep session-modes unset unless lock-screen behavior becomes a hard requirement.
- Build the extension and renderer as separate processes, but treat companion-process packaging as a later hardening topic.
- Keep GTK imports isolated to prefs.js and renderer-side GJS code.