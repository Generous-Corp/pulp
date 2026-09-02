---
name: gpu
description: Diagnose GPU health, discover bounded recipes, and collect typed numeric evidence without guessing from screenshots
---

Start with the cheapest truthful path:

```bash
pulp doctor gpu --json
pulp gpu recipes list --json
pulp gpu recipes list --symptom <exact-symptom-token> --json
pulp gpu recipes show <recipe-id> --json
```

Then run the selected closed recipe into a new path-confined artifact directory:

```bash
pulp gpu probe --recipe <recipe-id> --artifacts /tmp/pulp-gpu-evidence --json
```

Exit 0 is a measured pass, exit 1 is a completed measured failure, and exit 2
is unavailable or unverified. Runtime, internal-validation, and artifact
publication failures also use typed `unverified` JSON and exit 2; never read
them as completed measurements. Preserve the typed JSON, `gpu_evidence_id`,
adapter status/class, numeric oracle, and artifact digests. An unknown or
unverified adapter is not an authentic hardware claim, even when the backend is
Dawn/WebGPU. Never infer correctness from a screenshot alone.

Use `--negative-control` only to prove that the declared mutation fails for its
intended causal reason. Do not run GPU readback, validation, or artifact writing
on an audio thread. For a correlated trace, capture through the canonical
exact-instance trace lifecycle and use `/trace` with `gpu-probe` over the same
flushed `.pftrace`.

The recipe catalog is Pulp's product/tool workflow. It does not authorize a
generic rendering API or a DPR policy; those remain evidence-gated Vellum
follow-up work.
