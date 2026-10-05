# Spectr / GPU-NAM Forge exposure status

**Status: metadata-only; no named Forge consumer.**

The checked-in contract and example describe model selection and researcher
receipts for a future Spectr/GPU-NAM Forge consumer. They do not register a
`CustomNodeType`, add a Forge catalog row, or claim a public neural shared-GPU
adapter. `docs/status/forge-catalog.json` currently contains no
`spectr.gpu_nam` entry.

This is consistent with
[`gpu-wavenet-provider-plan-20260925.md`](gpu-wavenet-provider-plan-20260925.md):
GPU-NAM remains a staged worker integration with a continuously primed CPU
fallback until a public provider-owned neural adapter and its receipts exist.
The example's `exposure` object is therefore required to remain:

```json
{"status":"metadata_only","catalog_registered":false,"named_consumer":null}
```

Promotion requires a separately named downstream consumer, a Forge catalog
registration, installed-SDK/host acceptance evidence, and receipts for the
exact model/provider/device combination. Until then, changes belong in this
metadata/status surface only and must not widen Pulp/Vellum ownership or add
runtime/transport implementation.

The smallest named experiment candidate is recorded in
[`spectr-gpu-nam-paced-generation-receipt-20261002.json`](spectr-gpu-nam-paced-generation-receipt-20261002.json).
It is **Magenta RT2 as a paced producer/ring workload**, not a registered Forge
node. Its receipt is deliberately `blocked` with
`measurements_claimed: false`; the exact next step is a named-model,
named-Apple-Silicon run for 100,000 paced blocks, including a late-completion
fallback case. A passing receipt still does not register Forge automatically:
the public neural adapter and installed-SDK/host acceptance proof must be
reviewed first.

The 2026-10-02 local probe found no `/Users/danielraffel/.pulp/magenta` or
`models` directory and no `.mlxfn`, `.safetensors`, `.ckpt`, `.tflite`, or
`.onnx` artifact under the checked Pulp/Code roots. The `mrt2.mlxfn` and
`mrt2_state.safetensors` strings in `test/test_model_download.cpp` are generated
download fixtures, not an installed model. This is recorded in the receipt's
`local_probe` object; it is evidence of unavailability, not a reason to promote
the catalog.

The lane was rechecked at parent HEAD
`ff83c0e9fd147dc143af7228eac0e3298e84b35f` and is closed without catalog
promotion until a real artifact and paced receipt exist.
