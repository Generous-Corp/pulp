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
The dark graphite/lime design is painted through the Canvas/Skia surface; it
does not load the browser handoff's static full-frame `ui.js` image. That
browser-captured DesignIR remains the import/reference artifact, while the
shipping editor uses modular native tokens, controls and panel layout:

- `pulp_delay_ui_tokens.hpp` owns the visual tokens.
- `pulp_delay_controls.*` owns the custom knobs, faders, segmented controls,
  circulation field and action cards.
- `pulp_delay_editor.*` owns the panel composition and the complete one-control-
  per-parameter binding map.

Continuous controls use Pulp's gesture-aware two-way parameter bindings.
Segmented controls emit one-shot host gestures and hold scoped main-thread
listeners. Host automation updates are marshalled through the StateStore, and
the editor reconciles from the store before each paint so a deliberately
listener-silent preset/session deserialization appears on the next frame.

The knob renderer always uses the authored dark body. Its inactive 270-degree
track, lime active arc, pointer and display text all derive from the same
normalized value; focus adds a lime outer ring. No generic silver knob artwork
is used.

## Validation and screenshots

The UI tests cover all 25 bindings, host/RT updates, a routed drag with balanced
gesture begin/end, state restore, and Skia frames at feedback-normalized
`0`, `0.25`, `0.5`, `0.75` and `1`.

```sh
cmake --build build-delay --target pulp-delay-test pulp-delay-ui-shots
build-delay/examples/pulp-delay/pulp-delay-test '[pulp-delay][ui]'
build-delay/examples/pulp-delay/pulp-delay-ui-shots /tmp/pulp-delay-ui-shots
```
