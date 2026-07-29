# Pulp Delay

Pulp Delay is a stereo character delay built on `pulp::signal::CharacterDelay`.
Its processor exposes the complete 25-parameter contract used by the authored
1120×740 editor design.

The effect supports free and tempo-synchronised timing, linked or independent
right-channel timing, mono/stereo/ping-pong routing, four switchable character
engines, in-loop tone shaping, modulation, freeze, reverse, ducking and dry/wet
mixing.

## Native editor

`PulpDelayProcessor::create_view()` returns an authored native Pulp View tree.
The dark graphite/character-accent design is painted through the Canvas/Skia
surface; it does not load the browser handoff's static full-frame `ui.js` image.
That browser-captured DesignIR remains the import/reference artifact, while the
shipping editor uses modular native tokens, controls and panel layout:

- `pulp_delay_ui_tokens.hpp` owns the visual tokens.
- `pulp_delay_controls.*` owns the custom knobs, faders, segmented controls,
  circulation field and action cards.
- `pulp_delay_editor.*` owns the panel composition and the complete one-control-
  per-parameter binding map.

Continuous controls use Pulp's gesture-aware two-way parameter bindings.
Segmented controls emit one-shot host gestures and hold scoped main-thread
listeners. Host automation updates are marshalled through the StateStore.
Because preset/session deserialization deliberately restores state without
listeners, the shared editor idle pump consumes the restore edge on the UI
thread and reconciles every main-thread parameter listener from the canonical
StateStore snapshot. The same ordinary two-way bindings therefore cover user
edits, host automation and preset restore; the editor has no paint-time restore
protocol.

The knob renderer always uses the authored dark body. Its inactive 270-degree
track, character-coloured active arc, pointer and display text all derive from
the same normalized value; focus adds a character-coloured outer ring. No
generic silver knob artwork is used. The footer reports the current Mix and
Feedback parameter state; it is deliberately not presented as an audio output
meter. Header badges describe static editor capabilities rather than invented
host telemetry or preset state.

The authored accent follows Character through one immutable palette resolver:
Clean `#16DAC2`, Vintage `#A97BFF`, Tape `#B8E635`, and BBD `#FF4F4F`.
Reverse keeps its separate warning colour.

Mix and mono/stereo topology changes use per-sample 5 ms ramps. Character
changes crossfade between two pre-prepared engines without resetting either
engine in the audio callback, and rapid changes retain the most recently
requested character.

The Stereo Field mirrors the processor's timing branches. Sync swaps raw Time
editing for the selected beat division, Link swaps independent right timing for
Ratio or millisecond offset, and the read-only right-time field reports the
derived free-time result. Ping-Pong suppresses all ignored right/link controls
and replaces stored Crossfeed with an explicit `100% · PING PONG` override.

## Validation and screenshots

The tests cover all 25 bindings, host/RT updates, a routed drag with balanced
gesture begin/end, state restore, truthful state provenance, click-free mix and
routing automation, rapid character selection, a measured 192 kHz callback
budget, and Skia frames at feedback-normalized `0`, `0.25`, `0.5`, `0.75` and
`1`, plus one full frame for each Character palette. Screenshot checks decode
knob crops and require the changed-pixel geometry to contain each computed arc
endpoint.

```sh
cmake --build build-delay --target pulp-delay-test pulp-delay-ui-shots
build-delay/examples/pulp-delay/pulp-delay-test '[pulp-delay][ui]'
build-delay/examples/pulp-delay/pulp-delay-ui-shots /tmp/pulp-delay-ui-shots
```
