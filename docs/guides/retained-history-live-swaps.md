# Retained DSP history in live graph swaps

A live replacement may carry DSP history only when the replacement proves an exact
history key. Preset state (`save_state`) and DSP history are separate contracts:
delay lines, convolution tails, waveguide states, and physical-model integrators
must never be attached to a different algorithm or latency domain.

`NodeLiveSwapPolicy::retained_history.mode` selects one of five typed policies:

- `Clear` starts the replacement cold.
- `Adopt` restores an exact-key, bounded blob when one is available; otherwise it
  remains cold-start safe.
- `Crossfade` adopts the exact-key blob and uses the normal configured crossfade.
- `Reseed` asks the replacement for a deterministic seed and refuses if unsupported.
- `Refuse` fails the transaction when the key, state, or byte bound cannot be proved.

A failed history proof returns `LiveSwapFallbackReason::HistoryRefused` and leaves
ordinary eager-prepare available. The swap path never aliases old and new live
instances: the writer lock and snapshot retirement prove quiescence before the
old slot can be destroyed.

The executable receipt is in `test/test_retained_history_dspx03.cpp` under
`[dspx-03][retained-history]`; it covers exact history key adoption, clear, reseed,
identity mismatch, and bounded-state refusal. Format adapters may provide the
opaque `PluginSlot::serialize_dsp_state` and `restore_dsp_state` hooks; older
slots default to a cold start.

Each DSP cell owns its key provider. The key must include the algorithm identity,
state schema, and any sample-rate or latency assumptions that affect the blob.
An empty key intentionally disables adoption and makes `Refuse` fail closed.

The namespaced receipt also includes an independent impulse/tail recurrence
oracle over delay, convolver, waveguide, and physical-model keys. It checks that
an adopted nonzero tail survives the first post-swap sample while `Clear`
produces zero, independent of the implementation's state serializer.

The retained-history fixture drives the canonical `SignalGraph`/`PluginSlot`
replacement seam directly and uses the audio-analysis metrics over rendered
impulse buffers. `RenderScenario`/`HeadlessHost` cannot wrap a `PluginSlot`
instance without introducing a second adapter path, so this proof deliberately
keeps the host live-swap seam under test; the direct buffer analysis is the
independent audio oracle.
