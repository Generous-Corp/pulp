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
permanent `Generous-Corp/vellum` owner. An exact route wins. Without an exact
route, an initial-cut path already transferred to Vellum remains Vellum-owned;
other unlisted Pulp paths remain Pulp-owned, and unlisted Vellum paths remain
Vellum-owned. A result with both owners is `coordinated`, not permission to copy
either repository's product-specific code into the other.

The exact routes are independently frozen in
`references/approved-exact-routes.v1.json`, bound to the reviewed matrix,
amendment, and route-set digests. A projection cannot substitute a newly
self-hashed route set.

## Fail closed

Stop when the command reports a malformed projection, unsafe path, duplicate
route, repository-incompatible role, or claimed-owner conflict. Do not replace
an absent exact expansion with a prefix or glob guess. The accepted projection
authorizes maintenance routing only; it does not authorize Pulp consumption,
downstream cutover, or implementation before the corresponding Vellum
acknowledgement.

## GPU doctor boundary

Pulp owns the `pulp doctor gpu` CLI/MCP adapters, typed GPU-health evidence,
fixtures, tests, documentation, and skills. Those adapters may consume the
existing `Renderer3D`, `GpuCompute`, and `HeadlessSurface` interfaces.
`HeadlessSurface` itself is in the Vellum-authoritative transferred rendering
slice: do not change its header, implementation, or generic contract as part of
a Pulp GPU-doctor change. If the diagnostic cannot be implemented through the
existing interface, stop and route the required framework change to
`Generous-Corp/vellum`; do not add a Pulp-side duplicate or compatibility API.

The same ownership split applies to first-visible-frame GPU health. Pulp owns
the product measurement budget, closed result schema, capability-control
operation and executor adapter, CLI/MCP projection, Forge/DAW acceptance, and
Perfetto evidence interpretation. Generic frame-lifecycle spans, render-stage
identity, shader/pipeline cache instrumentation, or prewarm implementation are
Vellum work when their projected slice is framework-authoritative-transferred.
Keep nullable Vellum/source/shader and GPU/trace correlation seams in Pulp until
those producers exist; do not manufacture identities, promote incomplete event
captures, or advertise a live host capability before the exact product adapter
can return a validated snapshot.

For A3 receipts, distinguish capture integrity from instrumentation coverage.
Dropped or truncated available events invalidate every terminal disposition.
Named missing compile/upload/hidden/present/source/shader events may remain only
for a passing `no-change` or an over-budget `queue-B4-investigation`; the latter
must bind each missing event and argument to an exact path in the active
`framework-authoritative-transferred` `render-skia-dawn` slice. That route is a
request for post-adoption instrumentation and rerun, not authority to implement
the event or prewarm policy in Pulp.

A4 DPR orchestration, retained cell artifacts, deterministic analysis, and
protected-main publication receipts are Pulp-owned. `init-v2` must derive its
manifest from the fixed terminal A2T/A3 receipts; `finalize-v2` may consume only
nonce-bound runner snapshots, never a caller manifest/result/disposition. This
does not move generic DPR policy into Pulp: a freshly live-verified 84-plus-84
candidate only queues B5, which remains the separately reviewed Vellum adoption
boundary. The current zero-cell inconclusive result authorizes no route change.

The Pulp-owned live seam is `ControlGpuHealthProvider` plus
`ControlGpuHealthViewAdapter`: product hosts supply existing back-buffer and
`GpuSurface` callbacks, while exact generic present/source/shader/cache events
remain Vellum work. A capture-confirmed upper bound may populate a validated
snapshot but must keep the unratified startup verdict unverified. Route any
request for a true present hook or generic pipeline instrumentation to Vellum
instead of modifying generic window or render-lifecycle paths in Pulp.

The Pulp-owned A3 campaign runner may snapshot product-specific adapters,
validate 10+10 lifecycle/cache provenance, and name transferred instrumentation
gaps. That runner is not authority to add a missing generic producer in Pulp.
If the real adapter cannot source present, pipeline, upload, shader, or source
identity through an existing seam, leave the fields null, record the exact
`render-skia-dawn` gap, and route the post-adoption producer to Vellum. Never
make a warm label or a zero duration stand in for the missing boundary.

The checkout-owned `gpu_first_visible_a3_external_adapter.py` is within the
same Pulp evidence boundary: it may pin a role-specific executable, validate
its closed producer receipt, and run the independent blank/audio controls. It
does not move ownership of the producer's generic present, shader, pipeline,
upload, or cache events. When those facts are unavailable through existing
product seams, the producer must report the exact transferred gap rather than
adding a Pulp-side render hook.
The checked-in standalone, constrained-headless, REAPER, and Forge producers
are orchestration adapters inside that boundary. They may pin exact source,
product, host, and lifecycle-driver identities and reject missing endpoint
truth; their existence does not authorize a Pulp implementation of a missing
generic native-present or cache event.

Pulp also owns the A4 DPR runner, product-scenario adapters, evidence ingestion,
and the B5 dependency receipt. The runner may classify a measured candidate,
but B5 remains `waiting-trigger` until the adopted Vellum API refresh. Any
generic DPR policy, render-lifecycle instrumentation, or framework adapter
needed to act on that result originates in Vellum; do not implement it in this
Pulp evidence lane.

## Editing a pinned file stales its handoff row

`docs/status/gpu-vellum-handoff.yaml` pins a `revision` and an `object_id` per
path. `revision` is the most recent commit touching that path; `object_id` is
`git rev-parse <revision>:<path>`. Change a pinned file and its row still names
the previous blob, so `gpu_recipe_catalog.py` reports:

```
gpu-recipe-catalog: INVALID: handoff entries[N].pulp_paths[M] has stale revision/blob/tree identity
```

The failure surfaces through `gpu-recipe-catalog-selftest`, far from the file you
edited. It is easy to misread as someone else's flake: the test is named for the
GPU recipe catalog and the change that broke it may have nothing to do with GPU.
Do not expect it on one specific lane — it is an ordinary ctest, so it fails on
whichever full-suite lane finishes first, and it has been observed reddening both
the required `macos` gate and `Linux (x64) [github-hosted]`. A green macOS gate is
therefore not evidence that your pins are fresh. Run
`python3 tools/scripts/gpu_recipe_catalog.py` locally before pushing; it takes a
second and answers the question outright.

There is no `--write` mode. Refresh the affected rows only, to the most recent
commit touching each path and the blob at that revision. That is the identity
the catalog's own drift test plants staleness against, by deliberately using the
*second* most recent commit.

**Land the refresh as its own commit, never as an amend.** The pinned revision
is the commit that holds the edited file, so amending changes that SHA and
re-stales the pin you just fixed.

## Validate the contract

Run the closed eight-case suite and projection validator:

```bash
python3 .agents/skills/pulp-vellum-change-routing/scripts/test_route_change.py
python3 .agents/skills/pulp-vellum-change-routing/scripts/routing_evidence.py \
  validate --projection .github/vellum-ownership.json --require-expansion
```

Only the push-to-main path of `.github/workflows/vellum-routing-contract.yml`
emits the digest-bound `pulp-vellum-routing-contract-execution` artifact. Pull
requests and manual dispatches validate the contract without publishing release
evidence. That receipt is evidence for Vellum's release verifier; do not
hand-author or replay it.

A3 v2 terminal acceptance is Pulp integration evidence. Its attribution may queue Vellum follow-up, but terminal proof must bind exact transferred routes and may not manufacture missing generic instrumentation in Pulp.

The trusted PR gate validates a deterministic synthetic merge tree, but event
time and emergency-expiry checks are bound to the real PR source head supplied
with `--source-head`. A synthetic merge intentionally carries a fixed historical
timestamp and must never become the provenance clock for a newly added event.
