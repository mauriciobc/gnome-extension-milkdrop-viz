# Overview and Product Constraints

## Product definition

**gnome-milkdrop v2 — libprojectM Edition** is a GNOME-integrated music visualizer consisting of a native C renderer process and a thin GNOME Shell extension. The renderer process owns PipeWire capture, lock-free audio buffering, GTK4 window creation, OpenGL context usage through `GtkGLArea`, and `libprojectM` embedding. The Shell extension owns settings, process lifecycle, user controls, and recovery behavior.[cite:18][cite:16]

This document interprets the PRD as an architectural simplification: custom FFT, custom expression evaluation, manual shader pipeline construction, and custom preset parsing are intentionally removed from scope because `libprojectM` already provides those capabilities as a mature embedded engine.

## Design intent

The project is explicitly optimized for a short implementation sprint. Therefore, every design choice should favor low integration risk, GNOME compatibility, predictable lifecycle behavior, and minimal code that runs inside GNOME Shell.

The architectural center of gravity is the renderer binary, not the extension. GNOME guidance for extensions strongly supports this choice because extensions run in-process with the desktop shell and must avoid patterns that can destabilize the session or create review problems.[cite:16][cite:18]

## In-scope capabilities

- Audio capture from PipeWire monitor sources.
- Lock-free transfer of stereo PCM from the PipeWire thread to the render thread.
- Rendering through `libprojectM` inside a `GtkGLArea`-managed OpenGL context.
- Preset loading and cycling.
- Runtime control through a local Unix socket.
- GNOME Shell integration for spawn, stop, retry, and setting changes.
- Optional D-Bus exposure only if it does not complicate v2 delivery.

## Out-of-scope capabilities

The following remain out of scope unless a later approved design note explicitly expands scope:

- Re-implementing MilkDrop parsing, expression execution, FFT, warp/composite passes, or preset shader generation.
- Rendering directly inside GNOME Shell or Mutter.
- Custom Clutter or St-based visual composition in the extension.
- Complex multi-process orchestration beyond one renderer process and one extension client.
- Shipping private copies of GNOME platform libraries already expected from the distribution.

## Non-goals

The project is not trying to become a general-purpose visualizer framework, a new GNOME compositor effect, or a replacement for `libprojectM`. It is also not trying to expose every possible projectM feature on day one.

## Product constraints

### Process boundary

The GNOME Shell extension must remain a control plane. All expensive work must happen out-of-process because Shell extensions run inside the GNOME Shell runtime and should remain lightweight and easy to disable safely.[cite:16][cite:18]

### Input model

User-facing controls must follow GNOME HIG guidance: interfaces should be input-device agnostic, hover must not be required for essential actions, and anything available through a pointing device must also be possible from the keyboard.[cite:6][cite:9]

### Rendering ownership

GTK owns the OpenGL context via `GtkGLArea`; the renderer may only call OpenGL-dependent projectM functions when that context is current. This is a foundational constraint of the architecture and must not be weakened.

### Packaging model

The project is assumed to be dynamically linked against distro-provided runtime components. This matches normal GNOME/Linux packaging expectations and avoids stale bundled copies of a rapidly updated system graphics stack.

## Acceptance interpretation

For development purposes, the milestone is considered achieved when a preset renders on screen, responds to live audio, survives common Shell and process restarts, and can be controlled from the extension without restarting the GNOME session.
