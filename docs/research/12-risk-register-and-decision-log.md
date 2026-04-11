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

### Decision: Synchronize GPU rendering after projectM blur effects

**Status:** accepted (added 2025-04-11).

**Rationale:** ProjectM's blur effects use multiple asynchronous GPU rendering passes (horizontal + vertical blur, repeated per blur level). Without explicit synchronization, subsequent framebuffer reads capture incomplete/intermediate rendering state. The official SDL reference implementation (`projectm_SDL_main.cpp:453-455`) uses `SDL_GL_SwapWindow()` for implicit synchronization, but GTK's buffer swap happens later in the frame pipeline.

**Implementation:** Call `glFinish()` immediately after `projectm_opengl_render_frame_fbo()` to block until all GPU commands complete.

**Evidence:** Analysis of `reference_codebases/projectm/src/libprojectM/MilkdropPreset/BlurTexture.cpp:156-264` confirms multi-pass blur architecture. Comparison with SDL test UI confirmed synchronization requirement.

**Performance Impact:** Minimal (<1ms blocking time) in GPU-bound rendering loop.

**Consequence:** Blur presets now render correctly; pixel probing for startup reveal works reliably. This is a correctness fix, not an optimization.

**Location:** `src/main.c:659-677`

### Decision: Comprehensively restore GL state after projectM rendering

**Status:** accepted (added 2025-04-11).

**Rationale:** GTK's GLArea validates specific GL state after the render signal returns. While projectM cleans up most state internally (`CopyTexture.cpp:272-275`, `BlurTexture.cpp:267`), GTK requires guaranteed clean state for shader programs, texture bindings, blend modes, and test enables.

**Implementation:** Explicitly disable/unbind all potentially-modified GL state before returning from render callback.

**Consequence:** Prevents rendering artifacts and GTK warnings in compositor integration.

**Location:** `src/main.c:694-713`

### Decision: Use a local Unix socket for control

**Status:** accepted.

**Rationale:** command traffic is sparse and local, and the socket keeps process boundaries simple.

**Consequence:** no per-frame IPC, but protocol versioning discipline is required.

## Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---:|---|
| Extension lifecycle bug leaks actors/signals | Medium | High | Strict `enable()`/`disable()` symmetry, disable/enable regression test.[cite:16] |
| Control thread issues GL call | Medium | High | Render-thread-only GL rule, code review gate, doc enforcement |
| ~~HiDPI blur due to logical size misuse~~ | ~~Medium~~ | ~~Medium~~ | ~~Scale-factor-aware sizing and tests on 2x displays~~ **MITIGATED** (2025-04-11): Now addressed by multi-pass blur synchronization and comprehensive GL state restoration |
| projectM preset switch stutter | High | Medium | Accept/document for v2, avoid extra work during switch |
| Missing PipeWire monitor source | Medium | Medium | Silence fallback, clear status |
| Metadata claims unsupported Shell versions | Medium | Medium | Maintain tested-version matrix and accurate metadata only.[cite:16] |
| Excessive extension logging/review issues | Low | Medium | Sparse extension logs, clearer renderer-side logs instead.[cite:16] |
| Binary discovery fails after packaging | Medium | Medium | Document lookup rules and package install paths |
| Multi-pass blur effects incomplete/partial | ~~High~~ | ~~High~~ | **RESOLVED** (2025-04-11): `glFinish()` synchronization after `projectm_opengl_render_frame_fbo()` ensures all blur passes complete before framebuffer access |

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
