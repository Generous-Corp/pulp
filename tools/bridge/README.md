# Typed bridge contract gate

`bridge_contract_check.py` is the authoritative read-only check for the typed
editor bridge contract. It runs the safety audit before the existing generator
drift comparison:

```sh
python3 tools/bridge/bridge_contract_check.py
```

Use `--write` only when intentionally regenerating the checked-in outputs. The
gate rejects C++ reserved fields, transformed type or wrapper collisions,
empty transformed names, and TypeScript alias shadowing before it writes or
compares any output.

The canonical `set_parameter` command also generates a typed C++ decoder,
response builder, and `register_set_parameter` helper. The helper validates the
wire envelope and delegates parameter identity to the callback, so a plugin
can connect it to its real `StateStore` without hand-writing another JSON
parser. An empty key is rejected; a non-empty key that the callback cannot
resolve returns `{ok:true, accepted:false}` without changing state. The
callback therefore remains the explicit identity boundary rather than making
the bridge guess a `ParamID`; the production stable wire-key→`ParamID` map is
still a follow-up seam. `begin_gesture` and `end_gesture` remain contract
declarations until their host gesture lifetime is integrated.

The generated C++ header and TypeScript wrapper remain source-tree artifacts in
this slice. SDK packaging/export, an installed generation workflow, and
`@pulp/react` integration remain explicit follow-up boundaries.

The generated-client integration test uses Node's `--experimental-strip-types`
support (Node 22.6+, pinned to 22.15.0 in the native CI workflow) and fails
closed when that capability is unavailable.
