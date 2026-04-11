# GNOME 47-49 Notes

This file captures the version-sensitive details currently relevant to gnome-milkdrop.

## GNOME 47

- prefs.js methods getPreferencesWidget() and fillPreferencesWindow() are awaited.
- Keep prefs code async-safe and avoid assuming synchronous window construction.

## GNOME 48

- ExtensionBase gained getLogger(), which can be adopted later for cleaner logs.
- Some compositor helpers moved under global.compositor.
- Avoid relying on older Meta helper names without checking current APIs.

## GNOME 49

- Nested testing changed to dbus-run-session gnome-shell --devkit --wayland.
- Meta.WaylandClient changed. For subprocess ownership, use Meta.WaylandClient.new_subprocess() on GNOME 49.
- Meta.Window.get_maximized() was removed. Use Meta.Window.is_maximized().
- Meta.Rectangle was removed. Use Mtk.Rectangle if rectangle types are needed later.
- There were no material metadata.json, extension.js, or prefs.js format changes specific to GNOME 49.

## Implications For This Project

- The renderer launcher must branch between pre-49 and 49 Wayland client APIs.
- Any maximize or fullscreen policy code must handle both get_maximized() and is_maximized().
- Testing instructions and docs must use the devkit path for GNOME 49.
