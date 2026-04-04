# GNOME HIG Input and Accessibility

## Principle

GNOME’s HIG requires interfaces to work well across different pointing devices and physical abilities. Designs should be input-device agnostic, should not rely on hover to reveal essential actions or information, and should avoid physically demanding actions such as double-clicking or chording for core behavior.[cite:6]

This is directly relevant to gnome-milkdrop because its controls may be exposed through Shell UI and preferences, and those controls must remain usable regardless of whether the user is on a mouse, touchpad, touchscreen, keyboard-only workflow, or assistive setup.[cite:6][cite:9]

## Pointer and touch rules

The HIG states that click targets should be large enough for varied devices and abilities, and that controls available only on some pointing devices should not be exclusively relied upon.[cite:6]

Applied to this project:

- Start/stop visualizer must not depend on a secondary click.
- Next/previous preset must have a primary action path.
- Context menus may supplement but not replace the primary command surface.[cite:6]
- Hover-only reveal of pause or opacity controls is not acceptable for the sole access path.[cite:6]

## Keyboard completeness

GNOME’s keyboard guidance states that every action available with a pointing device should also be possible with the keyboard, and recommends testing apps with keyboard-only interaction.[cite:9]

For this project, keyboard completeness means:

- Panel or quick settings items must be keyboard navigable.
- Preferences controls must have labels, focus order, and activatable default widgets.
- Any context-menu-only action must also have another keyboard-reachable path.[cite:6][cite:9]

## Shortcuts

GNOME recommends using standard shortcuts where applicable, preferring mnemonic Ctrl-based combinations for non-standard actions, avoiding Alt for shortcut keys because of conflicts with access keys, and reserving Super for system shortcuts rather than application use.[cite:9]

For gnome-milkdrop, shortcuts should be conservative. A good default is to avoid global custom shortcuts in v2 unless there is a strong product reason and a tested settings UI for configuring them.

## Access keys and labels

GNOME describes access keys as a way to operate labeled controls using Alt combinations and recommends assigning memorable keys, while taking translation conflicts into account.[cite:9]

This matters primarily in preferences dialogs. Every form field in prefs should have a visible label, and controls should be grouped semantically rather than relying on placeholder text.

## Input-device-neutral language

The HIG explicitly says UI text should not reference specific input devices such as “move the mouse”.[cite:6]

Therefore, docs and UI copy should prefer:

- “Select” instead of “left-click”.
- “Open the menu” instead of “right-click the icon”.
- “Pointer” only when referring generically to pointing input.[cite:6]

## Secondary actions

GNOME says the secondary action should expose a context menu only when there is a relevant set of menu items, and it should not be used as the exclusive path for unrelated actions such as delete/remove.[cite:6]

Applied here, a context menu on the panel item can expose preset operations or diagnostics, but start/stop and core state should remain available through the main affordance.

## Gesture reservations

GNOME reserves three- and four-finger gestures for the system and also reserves drags from the top and bottom screen edges.[cite:6]

This project should therefore avoid any future Shell-side gesture design that conflicts with reserved system gestures.

## Accessibility baseline

Even though the visualization output itself is primarily decorative, the controls are functional and must remain accessible. At minimum:

- All controls must be reachable by keyboard.[cite:9]
- Focus order must be logical.
- Labels must describe settings clearly.
- Color alone must not communicate critical state.
- Status text should communicate failures such as “audio source unavailable” or “renderer not running”.

## Testing expectations

Every release candidate should be tested with:

- Keyboard-only navigation through the extension UI.[cite:9]
- No reliance on hover for essential actions.[cite:6]
- Large click/tap targets for panel and prefs controls.[cite:6]
- Clear fallback when a context menu is inaccessible or undiscovered.
