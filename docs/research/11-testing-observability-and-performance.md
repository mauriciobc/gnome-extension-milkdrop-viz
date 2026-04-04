# Testing, Observability, and Performance

## Test philosophy

Testing should focus first on integration boundaries, because the main project risks are not algorithmic novelty but lifecycle, rendering ownership, version compatibility, and cross-thread correctness.

## Core test matrix

### Environment matrix

At minimum, test across:

- Supported GNOME Shell versions actually declared in metadata.[cite:16]
- At least one modern Mesa-based Linux environment.
- At least one HiDPI setup.
- At least one PipeWire monitor-source-available setup.
- A no-monitor-source fallback scenario.

### Functional matrix

- Renderer starts from extension.
- Renderer stops cleanly from extension.
- Preset loads by path.
- Preset next/previous cycles work.
- Opacity updates live.
- Pause/resume works.
- Crash recovery restarts within policy.
- Disable/enable extension leaves no orphan UI or runaway timers.[cite:16]

## Performance budgets

The PRD defines these working goals:

- Stable 60 FPS target.
- Minimal non-projectM CPU time per frame.
- Audio-to-visual latency under the stated target.
- Zero steady-state IPC per frame.
- Acceptable preset switch stutter.

These targets should be treated as regression guards rather than marketing promises.

## Observability strategy

### Renderer observability

Include concise logs for:

- Startup configuration.
- PipeWire connection state.
- GL/context initialization success or failure.
- Preset load success/failure.
- Socket bind/listen failure.
- Abnormal shutdown.

Logs should be structured enough to debug but not noisy enough to impact the realtime path.

### Extension observability

The GNOME review guidance warns against excessive logging, so extension logs should be sparse and event-oriented: startup, subprocess exit, retry, socket failure, and invalid settings transitions.[cite:16]

## Manual debug procedures

Recommended command-line checks:

- Process presence and RSS.
- Socket existence/cleanup.
- `strace` or equivalent for unexpected steady-state IPC.
- `perf` or similar profiling to confirm most cost is inside projectM rather than glue code.

## Lifecycle regression tests

Every significant extension change should test:

1. Enable extension.
2. Start renderer.
3. Change settings.
4. Disable extension.
5. Re-enable extension.
6. Confirm no duplicate panel items, timers, signal handlers, or zombie processes remain.[cite:16]

## Accessibility tests

The extension and prefs UI should be tested keyboard-only because GNOME explicitly recommends verifying keyboard-only usability and requires that actions available to pointing devices also be available from the keyboard.[cite:9]

## Failure injection

Include deliberate tests for:

- Missing renderer binary.
- Socket bind failure.
- Invalid preset path.
- Renderer `kill -9` during operation.
- PipeWire source disappearance.
- GL initialization failure.

## Agent rules

Agents introducing new behavior must define how it is tested, observed, and failure-injected before considering the change complete.
