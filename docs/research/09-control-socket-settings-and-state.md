# Control Socket, Settings, and State Handoff

## Purpose

The Unix socket is the command channel between the GNOME Shell extension and the renderer. It exists to carry infrequent control events, not streaming data.

## Command model

The command protocol should remain binary, compact, and versioned by convention. Commands include preset operations, pause/resume, opacity changes, and status retrieval.

A stable command set for v2 includes:

- `preset-dir <absolute-path>`
- `next`
- `previous`
- `pause <on|off>`
- `shuffle <on|off>`
- `overlay <on|off>`
- `opacity <0.0-1.0>`
- `status`

## Threading rule

The control server must never call GL/projectM rendering operations directly from its own thread. It may parse commands, validate payloads, and update thread-safe pending state, but render-affecting work must be consumed by the render thread.

Preset loading is the canonical example: the control thread stores the requested path and flips an atomic pending flag; the render thread performs the actual load.

## Settings routing policy

Settings should be classified into one of three buckets:

| Category | Examples | Handling |
|---|---|---|
| Hot command | Next preset, pause, opacity | Send socket command immediately |
| Hot config | Preset directory, shuffle | Send a config command or request renderer-side rescan if supported |
| Restart-required | Device/monitor source, startup mode changes | Stop and relaunch renderer |

This policy keeps the Shell extension simple and avoids unnecessary process restarts.

## Validation rules

The control server should defensively validate:

- Payload length.
- Path string termination or bounded length.
- Float payload sanity for opacity.
- Enum range for commands.

Invalid commands should fail safely and never crash the renderer.

## Status model

A status response should include enough information for Shell UI and debugging without forcing expensive queries. At minimum:

- Running/paused state.
- Current preset name or identifier.
- Approximate FPS.
- Optional last known audio-source state.
- Optional error code/message class.

## Socket ownership and cleanup

The renderer owns the server socket lifecycle. On clean shutdown it should close the descriptor, remove the socket path if filesystem-based, and ensure a restart does not fail due to stale path state.

The extension owns the client lifecycle. On disable it should close client connections and stop any retry timers associated with reconnect behavior.

## Security and locality

The socket is a local control mechanism, not a remote API. It should be created with restrictive permissions appropriate for the user session. Any future expansion beyond local same-user control would require a separate security review.

## Failure handling

If the socket is unavailable:

- The extension should update status and may retry within a bounded backoff policy.
- The renderer should log a clear startup failure if bind/listen fails.
- Neither side should spin aggressively.

## Agent rules

Any new command added to the protocol must update protocol docs, parser validation rules, status expectations, and extension call sites in one change set.
