# Project Guidelines

## Code Style
- Keep implementation changes aligned with the planned split: native renderer in C and GNOME Shell integration in GJS.
- Prefer small, reviewable patches that preserve documented architecture invariants.
- Do not duplicate or modify code under `reference_codebases/` unless explicitly asked; treat it as read-only reference material.
- For extension lifecycle and GNOME review-sensitive behavior, follow `.github/skills/gnome-shell-extension-dev/SKILL.md`.

## Architecture
- Follow the two-process model defined in [PRD.md](../PRD.md) and [docs/research/02-system-architecture.md](../docs/research/02-system-architecture.md).
- Keep boundaries strict:
  - C renderer process owns PipeWire capture, ring buffer, projectM rendering, and control socket.
  - GJS extension owns GNOME Shell lifecycle, process supervision, settings routing, and compositor anchoring.
- Preserve invariants documented in research:
  - No OpenGL/projectM calls outside the render thread.
  - No per-frame IPC in steady state.
  - No heavy rendering work in the Shell process.

## Build and Test
- This repository is currently architecture and implementation-planning heavy. Validate assumptions against docs before introducing new build scripts.
- Planned native build flow (when renderer sources are present): `meson setup build && meson compile -C build`.
- For validation scope and regression expectations, use [docs/research/11-testing-observability-and-performance.md](../docs/research/11-testing-observability-and-performance.md).
- For packaging and dependency policy, use [docs/research/10-build-packaging-and-distribution.md](../docs/research/10-build-packaging-and-distribution.md).

## Conventions
- Keep Wayland/GNOME constraints explicit in design and implementation decisions; start from [docs/research/03-gnome-shell-and-mutter-context.md](../docs/research/03-gnome-shell-and-mutter-context.md).
- For extension code, enforce enable/disable lifecycle symmetry and avoid module-scope side effects. See [docs/research/04-gnome-shell-extension-development.md](../docs/research/04-gnome-shell-extension-development.md).
- For renderer-side GL integration and HiDPI behavior, use [docs/research/06-gtk4-glarea-projectm-integration.md](../docs/research/06-gtk4-glarea-projectm-integration.md).
- For audio capture and realtime ring-buffer behavior, use [docs/research/08-pipewire-audio-ring-buffer-and-realtime.md](../docs/research/08-pipewire-audio-ring-buffer-and-realtime.md).
- For control protocol, settings routing, and runtime state, use [docs/research/09-control-socket-settings-and-state.md](../docs/research/09-control-socket-settings-and-state.md).

## Documentation Map
- Product constraints and final decisions: [PRD.md](../PRD.md)
- Research index (canonical deep dives): [docs/research/](../docs/research/)
- Legacy notes (historical context only): [docs/legacy/](../docs/legacy/)