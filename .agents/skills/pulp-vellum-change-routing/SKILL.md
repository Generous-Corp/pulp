---
name: pulp-vellum-change-routing
description: Route repository-qualified changes across Pulp and Vellum using Pulp's exact ownership projection. Use when a change touches design import, Chromium authoring, DesignIR, visual harness, screenshot, Skia/Dawn, runtime assets, or another path represented by `.github/vellum-ownership.json`; when deciding whether work originates in Pulp or Vellum; or when validating the cross-repository routing contract.
---

# Route Pulp and Vellum changes

Run from the Pulp repository root. Treat the projection as authority; do not
infer ownership from similar directory names or broad path prefixes.

## Route changed paths

Pass the repository where each path currently lives:

```bash
python3 .agents/skills/pulp-vellum-change-routing/scripts/route_change.py \
  --repository Generous-Corp/pulp \
  --json \
  tools/import-design/browser_capture/capture.mjs
```

For a coordinated change spanning repositories, combine the default repository
with repeated `--change REPOSITORY:PATH` arguments:

```bash
python3 .agents/skills/pulp-vellum-change-routing/scripts/route_change.py \
  --repository Generous-Corp/pulp \
  --change Generous-Corp/pulp:tools/scripts/package_cli.py \
  --change Generous-Corp/vellum:cli/vellum \
  --json
```

The temporary private delivery repository `danielraffel/vellum` resolves to the
permanent `Generous-Corp/vellum` owner. An exact route wins. An unlisted Pulp
path remains Pulp-owned; an unlisted Vellum path remains Vellum-owned. A result
with both owners is `coordinated`, not permission to copy either repository's
product-specific code into the other.

## Fail closed

Stop when the command reports a malformed projection, unsafe path, duplicate
route, repository-incompatible role, or claimed-owner conflict. Do not replace
an absent exact expansion with a prefix or glob guess. The accepted projection
authorizes maintenance routing only; it does not authorize Pulp consumption,
downstream cutover, or implementation before the corresponding Vellum
acknowledgement.

## Validate the contract

Run the closed eight-case suite and projection validator:

```bash
python3 .agents/skills/pulp-vellum-change-routing/scripts/test_route_change.py
python3 .agents/skills/pulp-vellum-change-routing/scripts/routing_evidence.py \
  validate --projection .github/vellum-ownership.json --require-expansion
```

The push-to-main workflow `.github/workflows/vellum-routing-contract.yml` emits
the digest-bound `pulp-vellum-routing-contract-execution` artifact. That receipt
is evidence for Vellum's release verifier; do not hand-author or replay it.
