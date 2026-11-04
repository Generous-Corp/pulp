# Bounded typed trigger consumer

This consumer renders graph-produced onsets through a typed trigger edge, an
envelope, framework modulation, and a framework low-pass node. It freezes non-silent audio
samples and ordered `(epoch, sample, sequence, producer, token, source_port, kind)` records. It uses public
SDK headers and `Pulp::host` only.

The typed lane admits one framework onset producer (up to 16 detector lanes),
one framework envelope, and one port-zero unit-trigger edge. The prepared pair
runs sample by sample inside the canonical serial routed executor; surrounding
nodes keep their normal block schedule. The producer passes each audio input to
its corresponding output. The envelope accepts an optional audio input: connected
audio is multiplied by its envelope; an unconnected input produces the unit
envelope. External audio connections on both pair members survive contraction;
dependencies that become cycles are refused. The trigger is never encoded as MIDI.

Prepare and destroy candidates off the render thread. Installation and event
reset return `Busy` while a render owns the controller; a refused installation
keeps the candidate. Successful installation or reset advances the epoch and
clears the pair and graph routing/PDC/previous-block feedback history. The framework low-pass history resets with the pair. Opaque Custom callbacks
are refused because their generation storage cannot be bounded. Copied execution snapshots
cannot render typed plans. Use `process_checked` or the graph's normal `process`.

This first runtime surface admits audio I/O, Gain, and framework modulation/low-pass
surrounding nodes.
Hosted Processor/MIDI ingress nodes, arbitrary trigger producers, fan-out,
parallel/anticipated execution, sample regions, persistence and plugin parity
are outside this slice. It is not full G-5 delivery or realtime qualification.

The event resource report charges the shared 4096-by-8 record arena, controller,
a reserved authoring-descriptor page, and topology/frame-sized reservations for
each live, prepared or retained generation. Each reservation covers the candidate
object, per-node runtime and telemetry, both legacy and routed audio scratch,
feedback buffers, callback storage, alignment and temporary compiler plans.
Unused MIDI/SysEx scratch is omitted only for typed plans. Reservations are
checked against measured allocation bytes in maximum-frame preparation tests;
preparing a candidate cannot exceed the aggregate 2 MiB cap.
Borrowed trace views expire at the next render, reset or publication boundary.

Build against a freshly installed SDK from the same source revision:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PULP_SDK"
cmake --build build --parallel 1
ctest --test-dir build --output-on-failure
```
