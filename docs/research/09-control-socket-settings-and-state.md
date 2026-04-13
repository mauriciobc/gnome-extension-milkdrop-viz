# Control Socket, Settings, and State Handoff

## Purpose

The Unix socket is the command channel between the GNOME Shell extension and the renderer. It exists to carry infrequent control events, not streaming data.

## Command model

The command protocol should remain binary, compact, and versioned by convention. Commands include preset operations, pause/resume, opacity changes, status retrieval, and optional state snapshot/restore.

A stable command set for v2 includes:

- `preset-dir <absolute-path>`
- `next`
- `previous`
- `pause <on|off>`
- `shuffle <on|off>`
- `overlay <on|off>`
- `opacity <0.0-1.0>`
- `status`
- `save-state`
- `restore-state [key=value ...]`

Full details for `save-state` / `restore-state` are in the section below. See also tests: `tests/test_control_protocol.c`, `tests/test_control_state_flow.c`, `tests/test_state_persistence.c`.

### Preset directory and skip queues (renderer behaviour)

- **`preset-dir`:** The control thread writes the path into `pending_preset_dir` (mutex-protected) and sets a single pending flag. If many `preset-dir` lines arrive before the GL thread runs, each line updates that buffer — **the last path wins**; intermediate paths are not applied individually.
- **`next` / `previous`:** Each command increments a saturating counter on the renderer (cap `MILKDROP_PRESET_SKIP_QUEUE_MAX`, 64 per direction). The GL thread drains both counters once per frame (after playlist sync for that frame) and applies **`next_count - previous_count`** as a signed delta: positive means that many `play_next` steps, negative means `play_previous` steps. While the renderer is still in **deferred initial preset activation** (warmup gate after a non-empty playlist sync), skip steps are **not** applied yet; counts keep accumulating until the first real preset is activated.

## save-state and restore-state

### save-state

- **Request:** a single line `save-state\n` (no arguments).
- **Response:** one line terminated by `\n`, **without** an `ok` prefix (so clients can concatenate `restore-state ` + response for a round-trip parsed by `g_shell_parse_argv`).
- **Format:** space-separated `key=value` tokens:
  - `paused=on` or `paused=off`
  - `opacity=<0.0–1.0>` (three decimal places in practice, e.g. `opacity=0.625`)
  - `shuffle=on` or `shuffle=off`
  - If the renderer’s current preset directory (`AppData.preset_dir`) is non-empty: `preset-dir=<shell-quoted path>` (GLib `g_shell_quote`; responses typically contain the substring `preset-dir='`).
- If there is no preset directory set, the line **must not** contain `preset-dir=` (tests rely on omission).

### restore-state

- **Request:** `restore-state\n` with no further tokens: parse succeeds; no state fields are applied (no-op restore).
- **Request:** `restore-state` followed by one or more shell-parsed arguments, each of the form `key=value`:
  - Supported keys: `paused`, `opacity`, `shuffle`, `preset-dir`.
  - Unknown keys or invalid values → parse error (client should not send).
  - **Duplicate keys:** if the same key appears more than once, the **last** token wins.
- **Apply semantics:** only keys present are applied (partial restore). `preset-dir` uses the same mutex + `pending_preset_dir` + `preset_dir_pending` path as the `preset-dir` command.
- **Success response:** a line with prefix `ok` (e.g. `ok restore-state\n`).

### Round-trip

A typical client saves the full `save-state` response string and later sends:

`restore-state <saved line>\n`

(with the saved line trimmed of extra newlines so there is only one trailing `\n` on the wire).

### State subset (intentional)

| Included in save/restore | Not included (this protocol version) |
|--------------------------|--------------------------------------|
| `paused`, `opacity`, `shuffle`, preset directory | `overlay`, current `.milk` preset name, `fps` target, `rotation-interval` |

Extending the snapshot requires parser changes, tests, docs, and `controlClient.js` helpers in the same change set (see Agent rules).

### Conventions vs status

- **`status`** response uses `paused=` / `shuffle=` with numeric `0` or `1`.
- **`save-state`** uses `on` / `off` for `paused` and `shuffle`, consistent with the `pause` and `shuffle` **commands**.

### Buffer limits (renderer)

Defined in `src/control.c`:

- **Response buffer** (including `save-state` output): `MILKDROP_CONTROL_RESPONSE_MAX` (`MILKDROP_PATH_MAX * 2 + 512`). If serialization does not fit, the server responds with `err save-state-overflow\n`.
- **Receive buffer** (one command line): `MILKDROP_CONTROL_RECV_MAX` (`MILKDROP_PATH_MAX * 5`), so long `restore-state …` lines match test clients and the extension.

### Multi-monitor

Each renderer instance uses its own socket path (e.g. `milkdrop-<n>.sock`). Snapshots are **per process**; covering all monitors requires one save/restore pair per monitor index.

### Extension integration note (pause)

Restoring `paused` in the renderer does **not** update aggregate pause reasons in the Shell extension (`_pauseReasons` for fullscreen/maximized/MPRIS). Callers that need consistency with GSettings or pause policies should re-send `pause` or align extension state after `restore-state`.

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

Any new command added to the protocol must:

1. Update this document (`docs/research/09-control-socket-settings-and-state.md`).
2. Update the C parser (`src/control.c`) with validation and tests under `tests/`.
3. Expose a usable client API in `extension/milkdrop@mauriciobc.github.io/controlClient.js` **unless** the command is explicitly renderer-internal and documented as such (no Shell consumer).

UI in `prefs.js` or `extension.js` is recommended when the feature is user-facing, but is **not** required in the same change set as protocol + docs + client helpers.
