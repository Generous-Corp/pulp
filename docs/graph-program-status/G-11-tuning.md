# G-11 static tuning cookbook

State: **PARTIAL / installed-SDK fixture passed locally; gates and review pending**.

The `examples/g11-tuning-cookbook` installed-SDK consumer composes the public
`EqualTemperamentTuningProvider` with an owned `Processor` in `SignalGraph`.
Its exact-sample A4=442 Hz oracle, 32/17-frame partition comparison, invalid
note refusal, planted default-A4 negative, and failed-render refusal are the
bounded acceptance.
Configured against installed SDK `/Users/danielraffel/.pulp/sdk/1.6.0` on m3;
the governed Release consumer build and CTest passed all three cases (positive,
planted default-A4 negative, and injected failed-render refusal). Exact-head
gates and independent review remain
required before any PR. The installed SDK is an existing released prefix, so
this is consumer proof for its public API, not a current-main SDK packaging
receipt.

This does not prove Scala/KBM or MTS-ESP, sample-timed retuning, MIDI/MPE graph
ingress, realtime behavior, or Processor graph serialization.
