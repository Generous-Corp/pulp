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
