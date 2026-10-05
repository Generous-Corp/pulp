# Automation and modulation

Pulp uses one **base-vs-offset** value model at every delivery boundary: an authored **base**
value plus a host-owned **offset**. Timeline automation and ordinary
parameter writes update the base. Modulation routes update the offset. A
Processor reads the effective value (`base + offset`) and can clear the
offset at a block boundary without erasing automation.

`pulp::state::BaseOffsetValue` is the shared vocabulary for graph, Processor,
timeline, and control projections. `StateStore::get_base_offset()` exposes the
pair when a caller needs to preserve the distinction; `get_modulated()` remains
the bounded effective value used by DSP. A non-finite component is refused by
`validate_base_offset`, and an effective value outside its declared range is
reported as `OutOfRange` before a route publishes it.

Sparse parameter events and dense audio-rate modulation share the same target
and ordering rules. Dense delivery uses the fixed-capacity
`ModulationEventQueue`; overflow is explicit (`overflowed()` and the rejected
`push`) and never silently drops a route into a different rate. Audio-rate
sources are refused for control-rate targets by
`ModulationLaneRejectReason::AudioSourceRequiresAudioTarget`.

Timeline documents retain their modulation route and parameter target while
the playback compiler chooses sparse or dense delivery for the prepared block.
Generated/native/web projections may expose the route when they can preserve
base and offset exactly. A surface without that representation must report a
typed unsupported result; it must not replace the base with an offset or
silently downgrade dense delivery.

The route lifecycle is covered by the timeline command matrix (insert, rewire,
remove, inverse, stale expectation refusal), graph executor parity tests cover
dense routes over mixed block sizes, and Processor block tests cover dense
sidecar delivery at more than one sample rate. The approved minimal installable
DSPX-04 example host also proves live broker control: it binds a prepared
Processor-owned graph and reaches the canonical route operation through the
generic CLI and generated MCP projections, including lifecycle receipts and
bounded refusal for the prepared graph route. This live-control receipt is scoped to the namespaced example
host; Forge and commercial products still require their own equivalent proof.
Offline timeline CLI and MCP surfaces remain deferred until they can publish the
complete route lifecycle and dense overflow receipt.

The packet-owned matrix exercises audio-rate lane admission and ordered dense
delivery at 44.1, 48, and 96 kHz with 32, 64, and 128-frame blocks. Deferred
control surfaces carry a typed receipt with code `UnsupportedSurface` for
`automation.modulation.route`; the receipt names the missing route-lifecycle
and dense-overflow operation instead of implying a partial control API.
