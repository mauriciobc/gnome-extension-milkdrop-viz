# Risk Register and Decision Log

## Purpose

This document records the most important known risks, the rationale behind current decisions, and the conditions under which those decisions may be revisited.

## Decision log

### Decision: Use libprojectM instead of a custom MilkDrop pipeline

**Status:** accepted.

**Rationale:** the PRD intentionally removes a large volume of planned code by relying on projectM’s mature implementation of preset parsing, expression evaluation, FFT, and rendering behavior.

**Consequence:** v2 optimizes for delivery speed, lower code volume, and lower correctness risk, at the cost of less custom rendering control.

### Decision: Keep renderer out of GNOME Shell

**Status:** accepted.

**Rationale:** GNOME Shell extensions run inside the Shell runtime and should remain lightweight, lifecycle-safe, and easy to disable cleanly.[cite:16][cite:18]

**Consequence:** the extension is a controller, not a renderer.

### Decision: Use GtkGLArea as GL owner

**Status:** accepted.

**Rationale:** GTK-managed context ownership simplifies embedding and avoids custom context fights.

**Consequence:** GL-affecting projectM calls are restricted to the render path.

### Decision: Use a local Unix socket for control

**Status:** accepted.

**Rationale:** command traffic is sparse and local, and the socket keeps process boundaries simple.

**Consequence:** no per-frame IPC, but protocol versioning discipline is required.

## Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---:|---|
| Extension lifecycle bug leaks actors/signals | Medium | High | Strict `enable()`/`disable()` symmetry, disable/enable regression test.[cite:16] |
| Control thread issues GL call | Medium | High | Render-thread-only GL rule, code review gate, doc enforcement |
| HiDPI blur due to logical size misuse | Medium | Medium | Scale-factor-aware sizing and tests on 2x displays |
| projectM preset switch stutter | High | Medium | Accept/document for v2, avoid extra work during switch |
| Missing PipeWire monitor source | Medium | Medium | Silence fallback, clear status |
| Metadata claims unsupported Shell versions | Medium | Medium | Maintain tested-version matrix and accurate metadata only.[cite:16] |
| Excessive extension logging/review issues | Low | Medium | Sparse extension logs, clearer renderer-side logs instead.[cite:16] |
| Binary discovery fails after packaging | Medium | Medium | Document lookup rules and package install paths |

## Revisit triggers

The following conditions justify reopening major decisions:

- Need for in-window overlays or compositor-like integration that cannot be served by the current GTK window model.
- Need for projectM features that require additional companion libraries or playlist APIs.
- Requirement to support platforms or distributions where projectM 4.x packaging is inconsistent.
- Proven need for richer IPC than the current socket protocol supports.

## Change-control policy

Any change that touches one of these boundaries must update docs before merge:

- Process boundary.
- Thread ownership.
- GL context ownership.
- Extension lifecycle behavior.
- Settings routing between restart-required and hot-reload paths.
- Supported GNOME Shell versions.

## Agent rules

Agents should consult this file before expanding scope. If a proposal weakens an accepted decision, the proposal must explicitly state why the original rationale no longer holds.
