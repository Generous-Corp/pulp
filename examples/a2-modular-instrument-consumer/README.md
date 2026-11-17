# A2 modular instrument consumer

This installed SDK example builds a small modular instrument from one reusable
declaration and proves that the authored instance identity, its saved state,
and its Control timing survive a save and restore.

- **Declaration:** the `com.pulp.example.a2.strip` `SubgraphTemplate` wires a
  registered `StripFilter` Processor into a Gain node and a stateful Custom
  trim member (`com.pulp.example.a2.trim`, version 1), and declares one
  persisted parameter, `level`.
- **Instances:** the graph instantiates the template twice, `lead` (index 0,
  level 0.5, trim 1) and `echo` (index 1, level 0.75, trim 0.5), and connects
  them in series through their exposed ports.
- **Control time:** one `ControlIndexInput` drives an indexed coefficient table
  whose `value` output modulates both filters at audio rate.
- **Saved state:** `to_saved_subgraph_document` writes the saved-identity v7
  document. Every render restores that document into a fresh graph with
  `from_saved_subgraph_document`, the template, and an exact-version
  `ProcessorRestoreRegistry`.

The positive test checks the restored identities, indices and levels, each
trim member's exact Custom type, version and state, that a re-save keeps the
same semantic digest, and that one 16-frame block and a
3+5+8 partition through `process_control_checked()` both equal an independent
sample recurrence.

Seven planted defects must each be caught:

| Test | Defect | Expected result |
| --- | --- | --- |
| `unknown-template` | restore without the declaration | `subgraph_template_unknown`, graph left empty |
| `out-of-range-level` | saved `echo` level edited to 7.5 | restore refused, graph left empty |
| `edited-level` | saved `echo` level edited to 0.25 (in range) | loads, but disagrees with the recurrence |
| `skewed-clock` | second block's absolute start shifted by one | `WrongStart` |
| `missing-custom-type` | restore without registering the trim type | `subgraph_instance_mismatch`, existing graph untouched |
| `custom-version` | saved trim node names version 2 (also registered) | `subgraph_instance_mismatch`, existing graph untouched |
| `custom-state` | saved `echo` trim state edited to 9.0 | `subgraph_state_rejected`, existing graph untouched |

Configure this directory against an installed Pulp SDK (`-DCMAKE_PREFIX_PATH`)
and run its eight CTest cases. The example uses only installed public headers
and the `Pulp::host` CMake target.
