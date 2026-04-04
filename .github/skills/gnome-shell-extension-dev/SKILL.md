---
name: gnome-shell-extension-dev
description: 'Use when building or modifying a GNOME Shell extension in GJS, creating metadata.json, extension.js, prefs.js, GSettings schemas, nested-shell test flows, GNOME 47-49 compatibility logic, Wayland subprocess renderers, or review-readiness checks.'
argument-hint: 'Describe the GNOME extension task, target GNOME versions, and whether it touches shell process, prefs process, renderer subprocess, or packaging.'
---

# GNOME Shell Extension Development

Use this skill before changing extension lifecycle code, metadata, prefs, schemas, subprocess launch logic, or GNOME-version-sensitive Meta APIs.

## Core Rules

1. Keep the GNOME Shell process and the prefs process separate.
2. Only mutate shell state inside enable(). Undo all of it in disable().
3. Treat GNOME 47, 48, and 49 as distinct compatibility targets when touching Meta or shell internals.
4. Keep companion processes explicit, tracked, and cleanly shut down.
5. Prefer documented GNOME APIs first, precedent code second.

## When To Use

- Creating or editing metadata.json, extension.js, prefs.js, or schemas
- Building a shell-side manager that uses Meta, St, Shell, or Clutter
- Building a prefs window with GTK4 or libadwaita for an extension
- Adding subprocesses, sockets, or monitor-aware logic to a GNOME extension
- Checking GNOME 47, 48, or 49 upgrade risks before coding
- Preparing a local-first extension scaffold that can later be hardened for review

## Procedure

1. Confirm the target UUID, schema ID, and supported shell versions.
2. Read the rules in [GNOME extension rules](./references/gnome-extension-rules.md).
3. If the change touches version-sensitive APIs, read [GNOME 47-49 notes](./references/gnome-47-49-notes.md).
4. Keep shell-process imports limited to GNOME Shell and GI modules that are safe in shell.
5. Keep prefs-process imports limited to GTK4, Adwaita, Gio, GLib, and related GTK-side modules.
6. For subprocess renderers on Wayland, validate ownership and lifecycle with GNOME-version-aware Meta.WaylandClient handling.
7. Before shipping a new lifecycle feature, verify disable() disconnects signals, removes sources, and destroys tracked objects.
8. Test with a nested shell where possible before relying on the real session.

## Shell Process Checklist

- Use ESModule imports
- Do not create GObject instances in the constructor unless they are static and harmless
- Connect signals only in enable()
- Remove GLib sources in disable()
- Destroy or clear objects created in enable()
- Keep logging sparse and operational
- Do not import Gtk, Gdk, or Adw in the shell process

## Prefs Process Checklist

- Use GTK4 and Adwaita only
- Do not import Meta, Shell, St, or Clutter
- Bind settings through Gio.Settings
- Keep async setup compatible with GNOME 47+ awaited prefs methods

## Renderer Companion Checklist

- Track each subprocess explicitly
- Prefer clean shutdown over process sweeping
- Use runtime feature detection for optional GTK or GL features
- Keep IPC small, observable, and versioned
- Contain renderer crashes so they do not destabilize gnome-shell

## Deliverables

- Valid extension metadata and schema
- Minimal extension lifecycle that enables and disables cleanly
- Minimal prefs window that opens without shell imports
- Version-aware subprocess launch path for GNOME 47-49
- Short research notes when using undocumented or precedent-driven behavior

## References

- [GNOME extension rules](./references/gnome-extension-rules.md)
- [GNOME 47-49 notes](./references/gnome-47-49-notes.md)
