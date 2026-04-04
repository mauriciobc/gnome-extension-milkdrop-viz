# PipeWire Audio Source Research

Date: 2026-03-13

This note documents the research and implementation decision for selecting the correct capture source in PipeWire environments.

## Problem

The extension must react to what is currently playing on the main output device (speakers/headphones), not to microphone input.

In practice, this means we need monitor capture ("what-you-hear"), not generic input capture.

## Sources Reviewed

- WirePlumber linking policy:
  https://pipewire.pages.freedesktop.org/wireplumber/policies/linking.html
- WirePlumber wpctl manual:
  https://pipewire.pages.freedesktop.org/wireplumber/tools/wpctl.html
- PipeWire pw-cat / pw-record target semantics:
  https://pipewire.pages.freedesktop.org/pipewire/page_man_pw-cat_1.html
- PulseAudio-compatible pactl identifiers:
  https://man.archlinux.org/man/pactl.1.en

## Key Findings

1. Under WirePlumber policy, Stream/Input/Audio nodes without explicit target are linked to default or best Audio/Source.
2. In many systems, default Audio/Source is the microphone.
3. Therefore, generic capture fallback (for example, un-targeted pipewiresrc/auto source) can route to microphone.
4. In Pulse-compatible mode, @DEFAULT_MONITOR@ is the correct symbolic source for default output monitor capture.
5. The safest auto behavior for this project is strict monitor-only capture. If monitor capture is unavailable, fail safe to silence instead of microphone.

## Decision For gnome-milkdrop

Auto mode policy is now:

- Prefer pulsesrc device=@DEFAULT_MONITOR@.
- Do not fall back to pipewiresrc:auto.
- Do not fall back to autoaudiosrc.
- If monitor backend is unavailable, use stub silence and notify the user.

This guarantees the extension does not capture microphone in auto mode.

## Code Impact Summary

- audio source candidate selection in src/extension/audio.js was tightened to monitor-only behavior in auto mode.
- user-facing text in prefs and GSettings schema was updated to explicitly state no microphone fallback.
- tests were added to prevent future regressions in auto source selection behavior.

## Operational Notes

- Recommended setting for normal users: audio-source = auto.
- For advanced/manual routing, users may set a specific monitor source name (for example, alsa_output...monitor).
- If monitor capture backend is not available on a system, visuals continue without audio reactivity rather than using microphone.

## Runtime diagnostics added

The extension exposes audio pipeline status via `GetWindowStatus()` on D-Bus, including:

- configured source
- active source
- recent-signal flag
- restart attempts
- reprobe failures

This makes support/debug of PipeWire/Pulse routing issues possible without adding temporary debug patches.
