# Forge design-emission de-risk experiment

This directory is the durable Project C experiment from 2026-07-28. It is
deliberately Python + data only: Forge's C++ implementation was inspected but
not modified.

Run:

```sh
python3 profile_to_design_ir.py artifacts/<case>/final.json \
  --output artifacts/<case>/design_ir.json
python3 validate_emission.py artifacts/<case>/final.json \
  --concept concepts/<concept>.json --output artifacts/<case>/validation.json
```

`calls.jsonl` records provider/model/version, command, timing, exit status and
Claude/Codex session metadata without credentials. Raw provider event streams,
prompts, repair prompts, emitted artifacts, DesignIR translations, Skia renders,
blind labels and the final analysis are retained alongside it.
