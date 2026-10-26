---
breaking = false
summary = "SignalGraph edges pass samples through unchanged: a graph no longer turns -0 into +0 at an edge or at its output"
---

## What changed

- A graph node input fed by exactly one edge (with no plug-in delay
  compensation on it) now receives that edge's samples unchanged, instead of
  a copy computed as `0.0f + sample`. When the edge is its source's only
  reader, the input reads the source's buffer directly, with no copy at all.
  The graph's only output node copies into the output bus instead of adding
  into a zeroed one.
- Both the canonical executor and the reference walk do this, so they still
  match bit for bit.
- The only samples whose bits change are negative zeros. A graph used to hand
  them on as `+0`; it now hands on `-0`. So a chain of `Processor` nodes now
  produces exactly the bits that calling the same processors in sequence
  does.
- Inputs fed by several edges, or through a delay-compensation ring, still sum
  from zero. A graph with several output nodes still mixes them into the bus.

## Migration steps

- None for audio: `-0` and `+0` compare equal and sound the same.
- A test that compares a graph's output bits (`memcmp`, hashes, golden
  files) against a render made before this change may differ where the
  signal is exactly zero with a negative sign. Re-record it, or compare
  values instead of bits.
