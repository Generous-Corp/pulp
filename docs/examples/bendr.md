# Bendr — pitch / formant / freeze effect (separate repo)

Bendr is a real-time pitch and formant shifter with spectral freeze and a pitched
feedback delay, built on Pulp. It used to live here under `examples/bendr`. It has
grown into a full plugin — a native GPU editor in Pulp's **Ink & Signal** design
language, a custom XY pad and log-frequency spectrum, format packaging, and a
signed, notarized installer — so it now lives in its **own repository** and
consumes Pulp as an SDK:

> **https://github.com/danielraffel/pulp-bendr**

Keeping it out of the framework repo lets Pulp stay focused on reusable
capabilities rather than one large, domain-specific plugin. It also makes Bendr a
worked example of *building a real plugin against an installed Pulp SDK* — it
resolves Pulp through `find_package(Pulp CONFIG)` and depends only on Pulp's
public targets.

## What it demonstrates

Every processing block is a public primitive from `core/signal/`, so Bendr shows
what the shipped DSP library composes into:

| Primitive | Role |
|---|---|
| `RealtimePitchTimeProcessor` | pitch / formant shift, with built-in `FreezeHold` and `TransientPhasePolicy` |
| `PitchedFeedbackDelay` | tempo-syncable delay with an in-loop pitch shifter |
| `DryWetMixer` | latency-compensated dry/wet |

The editor is likewise assembled from stock `pulp::view` widgets over a single
`theme_from_preset("ink-signal")` call at the root. Only the XY pad and the
spectrum are custom canvas, because no stock widget models a semitone grid or a
log-frequency magnitude plot. Its numeric type-in fields use the shared
[caret](../reference/widgets.md#caret) surface — `pulp::view::CaretBlink` and
`paint_caret` — rather than a bespoke blink.

## Formats

VST3, AU v2, CLAP, Standalone.
