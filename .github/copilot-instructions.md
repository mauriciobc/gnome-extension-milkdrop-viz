# Project Guidelines

## Scope
- Treat [AGENTS.md](../AGENTS.md) as the canonical quick-start for commands, architecture boundaries, and daily conventions.
- Keep this file minimal and focused on high-priority guardrails.

## Guardrails
- Preserve the C renderer and GJS extension split.
- Do not duplicate or edit files under [reference_codebases](../reference_codebases) unless explicitly requested.
- Prefer small, reviewable patches.
- Keep GL and projectM work in render-thread callbacks only.
- Avoid per-frame IPC in steady state.

## Required References
- Product constraints: [PRD.md](../PRD.md)
- Research index: [docs/research](../docs/research)
- System architecture: [docs/research/02-system-architecture.md](../docs/research/02-system-architecture.md)
- GNOME Shell context: [docs/research/03-gnome-shell-and-mutter-context.md](../docs/research/03-gnome-shell-and-mutter-context.md)
- Extension lifecycle: [docs/research/04-gnome-shell-extension-development.md](../docs/research/04-gnome-shell-extension-development.md)
- GLArea and projectM integration: [docs/research/06-gtk4-glarea-projectm-integration.md](../docs/research/06-gtk4-glarea-projectm-integration.md)
- PipeWire and ring buffer: [docs/research/08-pipewire-audio-ring-buffer-and-realtime.md](../docs/research/08-pipewire-audio-ring-buffer-and-realtime.md)
- Control socket and state: [docs/research/09-control-socket-settings-and-state.md](../docs/research/09-control-socket-settings-and-state.md)
- Testing scope: [docs/research/11-testing-observability-and-performance.md](../docs/research/11-testing-observability-and-performance.md)
- projectM compliance checklist: [docs/research/13-projectm-integration-compliance.md](../docs/research/13-projectm-integration-compliance.md)
- GNOME extension skill: [.github/skills/gnome-shell-extension-dev/SKILL.md](./skills/gnome-shell-extension-dev/SKILL.md)