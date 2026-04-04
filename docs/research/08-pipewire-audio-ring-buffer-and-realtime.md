# PipeWire Audio, Ring Buffer, and Realtime Constraints

## Audio capture role

PipeWire provides the live audio stream used to drive the visualization. In this architecture, PipeWire capture is upstream of projectM and must remain lightweight, robust, and resilient to missing sources.

## Realtime rule

The PipeWire callback path must be treated as realtime-sensitive code. That means no blocking I/O, no dynamic allocation if avoidable, no mutex acquisition, no filesystem access, no command parsing, and no verbose logging.

The only required work is decoding or normalizing the incoming audio format as needed and writing stereo float pairs into the lock-free ring buffer.

## Ring buffer contract

The ring buffer exists to decouple the timing of the audio callback from the GTK render loop.

Producer contract:

- Writes interleaved stereo samples.
- Advances the write index with atomic release semantics.
- Drops data rather than blocking if the buffer is full.

Consumer contract:

- Reads available floats on the render thread.
- Consumes an even number of floats so stereo channel alignment is preserved.
- Advances the read index with atomic release semantics.

## Why lock-free matters

The render loop and realtime callback have different latency requirements and scheduling behavior. A lock-free single-producer/single-consumer ring is appropriate because it avoids priority inversion and keeps the audio path deterministic.

## Buffer sizing

The buffer should be large enough to absorb timing jitter between the audio and render threads, but not so large that visual response becomes sluggish. Buffer size tuning should be validated against the target latency goal rather than guessed.

## Silence fallback

If PipeWire monitor capture is unavailable, the renderer should not terminate unexpectedly. A silent or idle visualization state is preferable to crashing because it preserves Shell integration and troubleshooting workflow.

## Audio normalization and format assumptions

Any conversion from PipeWire sample format to the float format expected by projectM should be isolated and documented. Channel order, sample rate assumptions, and monitor source selection policy must be explicit.

## Overrun and underrun policy

- **Overrun**: drop the newest or current write operation according to the ring design; never block.
- **Underrun**: render using whatever recent samples exist, including none.

Visual continuity is less harmful than destabilizing audio or UI timing.

## Thread interaction rules

The audio thread must never:

- Touch GTK.
- Touch projectM directly.
- Open sockets.
- Scan preset directories.
- Emit user-facing state changes directly.

All downstream behavior should happen via the ring buffer and thread-safe status fields.

## Testing focus

Testing should verify:

- Correct stereo interleave behavior.
- No corruption when the render loop runs slower than audio callbacks.
- Graceful startup before audio becomes available.
- Graceful behavior when the source disappears mid-session.
