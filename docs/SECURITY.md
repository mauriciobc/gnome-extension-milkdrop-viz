# Security notes — milkdrop control socket

## Threat model

The renderer exposes a **Unix domain stream socket** per monitor instance:

- Default path pattern: `$XDG_RUNTIME_DIR/milkdrop-<monitorIndex>.sock`
- If `XDG_RUNTIME_DIR` is unset: `/tmp/milkdrop-<monitorIndex>.sock`

Commands adjust opacity, preset directory, shuffle, overlay, frame rate, preset rotation interval, pause, optional `load-preset`, and related state.

- The socket file is created with mode **0600** after bind so only the owning user can connect via the filesystem path.
- There is **no cryptographic authentication**. Any process running as the **same user** that can open the socket path can send control commands.
- This matches common patterns for per-user desktop services: trust boundary is the user session, not arbitrary code on the system.

## Recommendations

- Do not run untrusted code in your session that could target the socket path.
- Keep `XDG_RUNTIME_DIR` private (default on typical Linux desktop sessions).
- Packagers documenting hardening should mention that the socket is a local control plane, not a network service.

## Future hardening (optional)

Linux-specific options such as `SO_PEERCRED` could reject connections whose effective UID does not match the renderer’s. That would add complexity and platform-specific tests; it is not implemented today.
