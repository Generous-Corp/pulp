# G-5 ControlIndexInput filter

This installed SDK example authors a named Control path:

`ControlIndexInput.index → indexed-table.index → indexed-table.value →
Processor.Coefficient`.

The host supplies one index per sample through `process_control_checked()`. The
table maps each index to a one-pole lowpass coefficient, and the Processor
consumes the dense AudioRate modulation lane in `process_block()`. The positive
test compares one 16-frame block with a 3+5+8 partition and an independent
sample recurrence. The planted missing-edge case must return `RenderFailed`;
the swapped-index case must disagree with the independent recurrence.

Configure this directory against an installed Pulp SDK, then run its three
CTest cases. The example uses only installed public headers and CMake targets.
`--emit-v4` and `--emit-v5` print canonical Processor-only and typed-Control
graph files for compatibility checks.
