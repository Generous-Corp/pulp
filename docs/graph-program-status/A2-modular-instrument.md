# A2: modular performance instrument

Status: **PARTIAL. Installed-SDK consumer for declaration and instance
identity, saved state, and G-5 Control time is in this tree.**

## What the consumer proves

`examples/a2-modular-instrument-consumer` builds against an installed SDK
(`find_package(Pulp CONFIG REQUIRED COMPONENTS host)`) and uses only public
headers. The CTest entry `a2-modular-instrument-installed-sdk` installs the SDK
from the parent build, configures and builds the example, and requires its
five cases.

- **Declaration identity:** one `SubgraphTemplate`
  (`com.pulp.example.a2.strip`) holds a registered Processor and a Gain node
  with one declared parameter, `level`.
- **Instance identity and state:** two instances (`lead`, index 0, level 0.5;
  `echo`, index 1, level 0.75) are saved with `to_saved_subgraph_document` and
  restored with `from_saved_subgraph_document` plus an exact-version
  `ProcessorRestoreRegistry`. Restored identities, indices and levels are
  checked, and a re-save keeps the same semantic digest.
- **G-5 time and control:** a `ControlIndexInput` and indexed table modulate
  both filters at audio rate. Every render runs on a restored document through
  `process_control_checked()`; one 16-frame block and a 3+5+8 partition must
  both equal an independent sample recurrence.
- **Planted defects:** a missing declaration (`subgraph_template_unknown`), an
  out-of-range saved level (`subgraph_state_rejected`), an in-range edited
  level (output disagrees with the recurrence), and a skewed block start
  (`WrongStart`) are each detected.

## Glue budget

`main.cpp` is 245 lines. The `StripFilter` Processor (the instrument DSP) is 48
of them; the remaining 197 lines (includes, declaration, authoring, save and
restore, rendering, oracle, and the four planted controls) are the glue, under
the 200-line budget with little headroom.

## Not covered

- Parameterized nested templates, shared controls across more than one Control
  lane, nonlinear time (loop or seek of the Control clock), and live edits of a
  restored instrument.
- MIDI/MPE or voice ownership (A1 and G-6 own those).
- Realtime qualification, hosted plugins, and platforms other than macOS
  arm64.
