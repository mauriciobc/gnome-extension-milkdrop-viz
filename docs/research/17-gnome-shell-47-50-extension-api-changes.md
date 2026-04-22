# GNOME Shell Extension API Changes: Shells 47–50

## Purpose

Version-by-version reference for breaking changes that affect a background-rendering
extension targeting GNOME Shell 47, 48, 49, and 50 on Wayland. Focus is on APIs this
extension actually uses: compositor scene graph, window management, WaylandClient, and
the extension lifecycle.

## Baseline Requirement: ESM Modules (Shell 45+)

All shells in this range require ES Module syntax. Legacy `imports.*` no longer works.

```javascript
// Required form for all Shell 45+ extensions
import Gio from 'gi://Gio';
import Meta from 'gi://Meta';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import { Extension } from 'resource:///org/gnome/shell/extensions/extension.js';
```

`metadata.json` must list all targeted versions:
```json
{ "shell-version": ["47", "48", "49", "50"] }
```

---

## Shell 47

### Breaking Changes

**Clutter.Color removed.** Replace all `new Clutter.Color(...)` calls with `new Cogl.Color(...)`.

**Preference methods now async.**
```javascript
// Before
fillPreferencesWindow(window) { ... }

// After
async fillPreferencesWindow(window) { ... }
```

**PopupBaseMenuItem selection state.** CSS class `selected` → pseudo-class `:selected`.

**ControlsManagerLayout spacing.** The `_spacing` property was removed; spacing is now
passed as a parameter to `_computeWorkspacesBoxForState()` and `_getAppDisplayBoxForState()`.

### New APIs

- `org.gnome.desktop.interface.accent-color` GSettings key; accessible via
  `-st-accent-color` / `-st-accent-fg-color` CSS variables.
- `misc/util.js` now exports `fixMarkup()` (moved from `ui/messageList.js`).
- `ExtensionBase.getSettings()` available (was available earlier, but now stable API).

---

## Shell 48

### Breaking Changes

**`Meta.Compositor` namespace migration.** Window actor methods moved:

| Before | After |
|--------|-------|
| `Meta.get_window_actors()` | `global.compositor.get_window_actors()` |
| Direct `Meta.*` compositor calls | `global.compositor.*` or `Meta.Compositor.*` |

This directly affects background extensions that iterate window actors to find the
renderer window or to position the background actor.

**`Clutter.Image` removed.** Replace with `St.ImageContent`:
```javascript
// Before
const img = new Clutter.Image();

// After
const img = St.ImageContent.new_with_preferred_size(w, h);
```

**`Clutter.Stage.get_key_focus()` behavior change.** Now returns `null` when the stage
is unfocused (previously returned the stage itself). Guard against `null`.

**St widget `vertical` property deprecated.** Use `orientation`:
```javascript
// Before
widget.vertical = true;

// After
widget.orientation = Clutter.Orientation.VERTICAL;
```

### New APIs

**`ExtensionBase.getLogger()`** — structured logging with automatic name prefixing:
```javascript
const logger = this.getLogger();
logger.log('renderer started');
logger.warn('FBO incomplete');
logger.error('spawn failed');
```

---

## Shell 49

### Breaking Changes

**`Meta.Rectangle` removed.** Replaced by `Mtk.Rectangle`:
```javascript
import Mtk from 'gi://Mtk';
const rect = new Mtk.Rectangle({ x, y, width, height });
```

**`Meta.Window` maximize flag API overhauled.**

| Before | After |
|--------|-------|
| `window.maximize(Meta.MaximizeFlags.BOTH)` | `window.set_maximize_flags(...)` |
| `window.unmaximize(flags)` | `window.set_unmaximize_flags(...)` |
| `window.get_maximized()` | `window.is_maximized()` |

**`Meta.CursorTracker` pointer visibility API changed.**

| Before | After |
|--------|-------|
| `tracker.set_pointer_visible(false)` | `tracker.inhibit_cursor_visibility()` |
| `tracker.set_pointer_visible(true)` | `tracker.uninhibit_cursor_visibility()` |

**Clutter gesture actions removed.**

| Before | After |
|--------|-------|
| `new Clutter.ClickAction()` | `new Clutter.ClickGesture()` |
| `new Clutter.TapAction()` | (use ClickGesture) |
| `new Clutter.LongPressAction()` | `new Clutter.LongPressGesture()` |

**`Meta.WaylandClient` API redesigned.** This is the most significant change for this
extension. See [16-meta-wayland-client-lifecycle.md](16-meta-wayland-client-lifecycle.md)
for the full migration. Summary:

| Before (≤ 48) | After (49+) |
|---------------|-------------|
| `client.spawnv(launcher, argv)` | `Meta.WaylandClient.new_subprocess(ctx, launcher, argv)` |
| `client.hide_from_window_list(win)` | `win.hide_from_window_list()` |
| `client.show_in_window_list(win)` | `win.show_in_window_list()` |
| `client.set_type(win, type)` | `win.set_type(type)` |

### New APIs

- `Main.brightnessManager` — new module at `misc/brightnessManager.js`.
- `Meta.Window.set_type()`, `hide_from_window_list()`, `show_in_window_list()`.
- `Meta.WaylandClient.new_subprocess()` constructor.
- `Meta.WaylandClient.owns_window()` for ownership checking.

---

## Shell 50

### Breaking Changes

**X11 backend removed from Mutter.** No more `Meta.is_wayland_compositor()` returning
`false` on a native session. XWayland still exists for X11 apps, but the native Mutter
backend is Wayland-only. Extension code that branched on X11 should be simplified or
removed.

**`GdkEventKey.keyval` renamed to `hardware_keycode`.** Affects any extension that
handles raw key events. GNOME Shell input code has been updated; extension-level key
handling must follow.

**`libsigcplusplus` and `graphene` removed** as GNOME Shell build dependencies. Extensions
that vendored or linked against these indirectly may need to update.

### Practical Notes for This Extension

- Shell 50 finalizes the Wayland-only world assumed since Shell 40. Remove any remaining
  X11-guard code branches.
- The `Meta.WaylandClient` API from Shell 49 is stable in Shell 50 — no further changes
  documented.
- `Mtk.Rectangle` is the rectangle type going forward; no `Meta.Rectangle` shim exists.

---

## Version Compatibility Matrix

| API / Feature | 47 | 48 | 49 | 50 |
|---------------|----|----|----|----|
| ESM required | ✓ | ✓ | ✓ | ✓ |
| `Clutter.Color` | ✓ | removed | — | — |
| `Clutter.Image` | ✓ | removed | — | — |
| `St.ImageContent` | — | added | ✓ | ✓ |
| `Meta.Rectangle` | ✓ | ✓ | removed | — |
| `Mtk.Rectangle` | — | — | added | ✓ |
| `Meta.get_window_actors()` | ✓ | removed | — | — |
| `global.compositor.*` | — | added | ✓ | ✓ |
| `Meta.WaylandClient.spawnv` | ✓ | ✓ | removed | — |
| `Meta.WaylandClient.new_subprocess` | — | — | added | ✓ |
| `win.hide_from_window_list()` | — | — | added | ✓ |
| `widget.vertical` | ✓ | deprecated | — | — |
| `Clutter.Orientation` | — | added | ✓ | ✓ |
| `ExtensionBase.getLogger()` | — | added | ✓ | ✓ |
| X11 native backend | ✓ | ✓ | ✓ | removed |

---

## metadata.json Shell Version Declaration

The extension must explicitly declare all supported versions. GNOME Extensions portal
disables extensions on unlisted shell versions:

```json
{
  "uuid": "milkdrop@mauriciobc.github.io",
  "shell-version": ["47", "48", "49", "50"],
  "name": "MilkDrop Visualizer",
  "version": 1
}
```

## Canonical Sources

- Shell 47 upgrade guide: https://gjs.guide/extensions/upgrading/gnome-shell-47.html
- Shell 48 upgrade guide: https://gjs.guide/extensions/upgrading/gnome-shell-48.html
- Shell 49 upgrade guide: https://gjs.guide/extensions/upgrading/gnome-shell-49.html
- Shell 50 upgrade guide: https://gjs.guide/extensions/upgrading/gnome-shell-50.html (check when published)
- GNOME 50 release notes: https://release.gnome.org/50/developers/
- Mutter source: https://gitlab.gnome.org/GNOME/mutter
- GJS extensions overview: https://gjs.guide/extensions/overview/architecture.html
