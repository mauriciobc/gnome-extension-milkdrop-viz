# GNOME Extension Rules

This file distills the official GNOME extension guidance into the rules that matter for this project.

## Lifecycle

- metadata.json and extension.js are the required core files.
- GNOME Shell extensions use ESModules on GNOME 45 and later.
- The extension constructor is not the place to connect signals, create widgets, or mutate GNOME Shell state.
- enable() is where signals, sources, and shell-side objects are created.
- disable() must disconnect signals, remove sources, destroy objects, and undo shell mutations.

## Process Boundaries

- extension.js runs in the gnome-shell process.
- prefs.js runs in a separate GTK process.
- Do not import Gtk, Gdk, or Adw in the shell process.
- Do not import Clutter, Meta, St, or Shell in prefs.

### C renderer and two-process model

- A separate **C renderer process** (the renderer binary) is supervised from **extension.js**. It is responsible for PipeWire audio capture, ring buffer management, and projectM/OpenGL rendering.
- **Control IPC**: the extension talks to the renderer over a **control socket** (commands and settings). There is **no per-frame IPC in steady state**—only control messages, not per-frame render or audio handoff through the socket.
- **Strict boundaries**: extension.js and prefs.js must **never** call OpenGL or projectM or perform heavy rendering. prefs.js remains in the **separate GTK prefs process** (see above); only the C renderer owns the GL context and projectM.
- The **GJS extension** side of the two-process model should focus on **lifecycle supervision**, **GSettings routing** to the renderer, and **compositor anchoring** (wallpaper-layer integration, Wayland client ownership), not on GPU work.

## Metadata And Schema

- Use a review-safe UUID with a namespace under project control.
- shell-version must list only supported stable releases.
- settings-schema should match a real schema ID under org.gnome.shell.extensions.*.
- session-modes should be omitted unless the extension truly needs more than user mode.

## Review-Sensitive Areas

- External scripts and binaries are discouraged by GNOME review.
- If a companion process is necessary, it must be carefully spawned and cleanly terminated.
- Logging should be limited to meaningful operational messages and errors.
- Code must stay readable and reviewable.

## Testing

- For GNOME 49, nested shell testing uses gnome-shell --devkit --wayland.
- Use journalctl and the nested shell first when validating shell-side lifecycle changes.
