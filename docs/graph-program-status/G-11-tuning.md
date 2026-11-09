# G-11 static tuning cookbook

State: **PARTIAL / exact-head installed-SDK receipt retained**.

The `examples/g11-tuning-cookbook` installed-SDK consumer composes the public
`EqualTemperamentTuningProvider` with an owned `Processor` in `SignalGraph`.
Its exact-sample A4=442 Hz oracle, 32/17-frame partition comparison, invalid
note refusal, planted default-A4 negative, and failed-render refusal are the
bounded acceptance.
The governed Release consumer build and CTest passed all three cases (positive,
planted default-A4 negative, and injected failed-render refusal) against the
exact-head SDK recorded in
[`g11-current-head-20261109`](../evidence/g11-current-head-20261109/). The
receipt binds A4=442 Hz, 32/17-frame partition parity, invalid-note refusal,
and the failed-render refusal to source `3fad8b42a44d496361f97eeb469639e3a2b9e3c6`
and marker SHA-256
`cd4a21dd1ee66ae354c0c2c68e73cb0ce246b9bf045f553d86ccbfcc3e94f7ae`. The
marker is a `source-install` with `build_profile=release` and
`distribution_eligible=false`, not a development or experiment marker. This
is consumer proof for the public API, not realtime or sample-timed retuning
proof.

This does not prove Scala/KBM or MTS-ESP, sample-timed retuning, MIDI/MPE graph
ingress, realtime behavior, or Processor graph serialization.
