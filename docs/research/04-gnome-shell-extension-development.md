# GNOME Shell Extension Development

## Development baseline

GNOME’s extension development guide shows the modern structure for extensions, including `metadata.json`, `extension.js`, and clear lifecycle entry points. For recent GNOME versions, extensions use ES modules and should follow the current GJS guide rather than older legacy patterns.[cite:18][cite:3]

## Required directory structure

A minimal project layout for this extension is:

```text
extension/
└── milkdrop@mauriciobc.github.io/
    ├── extension.js
    ├── prefs.js
    └── metadata.json
```

The directory name must match the UUID used in metadata, because GNOME extension loading relies on that identity convention.[cite:3]

## Lifecycle rules

The review guidelines are explicit that extensions must avoid doing work too early. Signals, objects, timers, UI actors, and other live runtime resources should be created in `enable()` and destroyed in `disable()`, not in global scope or constructors that survive disable/enable cycles.[cite:16]

Practical rules for this project:

- No subprocess spawn at module import time.[cite:16]
- No signal connection at module import time.[cite:16]
- No `Mainloop.timeout_add` or equivalent source left uncleared across disable/enable cycles.[cite:16]
- No Shell widgets left attached after disable.[cite:16]

## Module-scope policy

Allowed at module scope:

- Imports.
- Constants.
- Lightweight helpers with no side effects.
- Class definitions.

Disallowed at module scope:

- Process creation.
- Shell UI insertion.
- Settings listeners with side effects.
- Timer startup.
- File descriptors or sockets that persist across disable.

## Shell vs prefs separation

The review guidelines state that `Gtk`, `Gdk`, and `Adw` must not be imported into `extension.js`, while `St`, `Shell`, `Meta`, and related Shell-side libraries must not be imported into `prefs.js`.[cite:16]

This project should therefore adopt a hard separation:

| File | Allowed concerns | Disallowed concerns |
|---|---|---|
| `extension.js` | Shell integration, menus, status, settings listeners, subprocess control, socket client | GTK widgets, Adwaita preferences UI |
| `prefs.js` | Preferences widgets, validation, descriptions, bound settings | Shell actors, panel items, Shell runtime objects |

## Metadata rules

`metadata.json` should include:

- Stable UUID.
- Accurate name and description.
- URL to project/source repository.
- Exact supported Shell versions only.[cite:16]

Do not claim support for GNOME versions that have not been tested.[cite:16]

## Process spawning guidance

The extension review guidance treats scripts and binaries as an area that deserves care.[cite:16] For this project, subprocess use is justified because the real rendering work belongs outside Shell, but the extension must keep the contract auditable and predictable.

Document and implement the following:

- Exact executable lookup strategy.
- Environment assumptions.
- Socket path derivation.
- Retry/backoff policy.
- Normal shutdown signal.
- Forced kill timeout.
- Behavior when binary is missing.

## Settings strategy

The extension should subscribe only to settings that affect Shell-side behavior directly. Settings that affect the renderer should be translated into socket commands when possible, reserving full process restart for changes that fundamentally alter capture or window topology.

A recommended split:

- **Restart-required settings**: monitor source, overlay/window mode, environment-sensitive startup flags.
- **Hot-reload settings**: preset directory, shuffle, pause, opacity, next/previous preset.

## UI affordances

The extension UI should favor standard GNOME patterns, such as a panel menu or quick settings item, and must not depend on hover-only controls because GNOME HIG warns against using hover to reveal essential actions or information.[cite:6]

All essential actions should remain keyboard-activatable, consistent with GNOME keyboard guidance.[cite:9]

## Review-sensitive practices

The extension should avoid:

- Excessive logging.[cite:16]
- Minified or obscured code.[cite:16]
- Unsafe monkey patching outside well-understood GNOME extension patterns.
- Poor cleanup of signals and actors.[cite:16]
- Claimed compatibility without testing.[cite:16]

## Recommended implementation checklist

- Create settings object in `enable()`, clear it in `disable()`.
- Create panel/menu UI in `enable()`, destroy it in `disable()`.
- Spawn renderer on demand or on enable depending on settings.
- Watch subprocess exit and update status.
- On disable, stop reconnect timers, disconnect settings signals, close sockets, destroy UI, and terminate renderer if owned.
- Keep all state handles nullable and clear them after destroy/disconnect.

## Agent rules

Any agent editing the extension must preserve lifecycle symmetry. Every resource added in `enable()` must have a corresponding, verified destruction path in `disable()`.
