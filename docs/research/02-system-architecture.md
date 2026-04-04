# System Architecture

## Topology

The system consists of two main runtime units:

1. A native renderer binary written in C.
2. A GNOME Shell extension written in GJS.

This split aligns with GNOME extension guidance because Shell-side code should stay narrow in scope and carefully manage lifecycle, while heavyweight work belongs outside the Shell process.[cite:16][cite:18]

## Responsibility split

| Component | Responsibilities |
|---|---|
| Renderer binary | PipeWire capture, ring buffer, GTK application/window, `GtkGLArea`, `libprojectM` lifecycle, socket server, preset indexing, opacity changes, status reporting |
| GNOME Shell extension | Read/write settings, spawn and stop renderer, reconnect after crashes, issue control commands, expose user affordances inside GNOME Shell |
| Preferences UI | Present settings using GTK/Adwaita-side APIs rather than Shell-side UI classes, with no Shell imports in prefs code.[cite:16] |

## Why this split exists

The split isolates failure domains. If the renderer crashes, the extension can report, retry, or remain idle without taking down the Shell. If the extension misbehaves, it can be disabled separately. This is more robust than putting rendering or audio code inside the Shell runtime.[cite:16][cite:18]

## Thread model

The renderer process contains at least three behavioral domains:

- **Audio thread**: fed by PipeWire callbacks; writes stereo float samples into the lock-free ring buffer.
- **Render/UI thread**: GTK main thread; owns `GtkGLArea`, drains audio, feeds projectM, switches presets, and draws a frame.
- **Control thread**: blocks on the Unix socket and updates shared state through atomics or main-thread-safe handoff.

The rule is strict: only the GTK render path may perform OpenGL/projectM rendering calls; the control thread must never issue GL calls directly.

## Frame flow

A single frame follows this logical pipeline:

1. PipeWire callback receives audio.
2. Audio callback pushes interleaved stereo floats into the ring.
3. GTK render callback drains available samples.
4. Samples are passed to `projectm_pcm_add_float(...)`.
5. Any pending preset change is consumed from an atomic handoff.
6. `projectm_render_frame(...)` draws into the currently bound GL target.
7. GTK-sensitive GL state is restored if necessary.

This flow minimizes per-frame IPC and keeps the hot path inside one process, which reduces latency and synchronization complexity.

## IPC policy

The Unix socket is the only required control channel. It is not a per-frame transport. The architecture assumes zero IPC during steady-state rendering except for occasional commands such as preset switching, pause/resume, opacity changes, and status requests.

## Shared state rules

Shared state must be partitioned into three categories:

- **Realtime-owned state**: audio write indices and sample storage.
- **Render-owned state**: current projectM handle, frame timing, GTK window state.
- **Control-owned state**: incoming command parsing, pending preset path, status request handling.

Any shared field crossing domains should use atomics or a main-loop handoff strategy. Mutexes must not be introduced in the realtime path.

## Failure domains

### Extension failure

Because the extension runs inside GNOME Shell, a serious bug can affect the desktop session. For that reason, the extension must not do heavy work and must always fully tear down objects and signal handlers on disable.[cite:16]

### Renderer failure

Renderer crashes should be handled as recoverable process failures. The extension should be able to detect abnormal exit, present status, and retry within a bounded policy.

### Audio source failure

If no monitor source is available, the renderer should stay alive, surface the condition, and fall back to silence rather than crash.

## Architecture invariants

The following rules are invariant unless explicitly superseded:

- No GL from non-render threads.
- No heavy rendering work inside GNOME Shell.
- No IPC in the steady-state frame loop.
- No custom shader pipeline in v2.
- No custom FFT or expression VM in v2.
- No extension-side GTK imports.
