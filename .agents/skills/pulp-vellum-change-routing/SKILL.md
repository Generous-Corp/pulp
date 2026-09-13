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

`gpu_recipe_catalog.py` has no `--write` mode and is deliberately validate-only.
The generator that owns those identities is a separate tool:

```bash
python3 tools/scripts/gpu_handoff_provenance.py check                       # every stale row, and the repair command
python3 tools/scripts/gpu_handoff_provenance.py write --receipt             # regenerate, and refresh the receipt
```

Pass `--receipt`. The published receipt is asserted against the ledger's bytes,
so regenerating without it leaves a second gate red for the next reader.

`gpu_recipe_catalog.py` will not catch that omission. It validates the ledger's
own identities and reports `OK` while the receipt still names the previous
ledger bytes, so the check recommended above is green in exactly the state this
paragraph warns about. Prove the receipt separately — re-run `write --receipt`
and confirm it rewrites nothing, or compare the receipt's `handoff_sha256`
against `shasum -a 256 docs/status/gpu-vellum-handoff.yaml`.

`check` names each stale row, its path, and the field-level correction, so a
drifted pin no longer has to be located by hand. `write` derives `revision`,
`object_id`, and `object_type` for every declared path from a single commit
(`--source-commit`, default `HEAD`, which must be an ancestor of `HEAD`), so a
regenerated ledger can never mix trees from different revisions. It refuses to
run against an unclean checkout, and it refuses to emit anything
`gpu_recipe_catalog.py` would reject, so the fail-closed validator stays the
authority on acceptance.

Do not hand-edit these rows. Hand editing is what turned a one-row correction
into dozens of stale identities: the rows are denormalized, one path can appear
in several packages, and a path's owning revision moves whenever any commit
touches it.

**Land the refresh as its own commit, never as an amend.** The pinned revision
is the commit that holds the edited file, so amending changes that SHA and
re-stales the pin you just fixed.

**Expect one cascade, and re-run the checker after the refresh.** This SKILL.md
is itself a pinned path, so editing it to record a gotcha stales its own row:
fixing one pin creates the next. The sequence terminates, because a pin-refresh
commit touches only the YAML, and the YAML excludes itself from the inventory.
Land the file edits first, then regenerate in a single following commit. Re-run
`gpu_recipe_catalog.py` after the refresh rather than before, or the second
stale row goes out unseen. Note that `gates.sh` and the pre-push hook do **not**
run this check, so a clean `gates: ✓ all gates pass` says nothing about your
pins.

The drift check is also a ctest, `gpu-handoff-provenance-selftest`, so an
unregenerated ledger fails locally and in CI with the repair command in the
failure message rather than only as a stale-identity report.

**Write the receipt in the same run as the ledger, before you commit either.**
`write --receipt` stamps `source_commit` with whatever HEAD is when it runs, and
`test_published_receipt_binds_the_checked_in_ledger` re-derives the ledger from
that commit and compares bytes. A bare `write` that is then committed leaves no
way to fold the receipt into that commit afterwards: regenerating stamps the
commit you just made, and `--amend`ing the receipt into it orphans that SHA, so
the receipt names a commit that is no longer an ancestor of HEAD, and every
further amend repeats it. Regenerate from a clean tree with `--receipt` first,
then commit the ledger and the receipt together. To recover from a bare `write`
that already landed, `git reset --soft` back to the commit that owns the edited
files, restore the ledger, and regenerate with `--receipt`.

Nothing in the fast path catches that omission. `gates.sh`'s `gpu-handoff pin
freshness` gate only asserts the ledger was **touched**, so a bare `write` turns
it green, and `gpu_recipe_catalog.py` validates the ledger's own identities and
also reports `OK`. The stale receipt surfaces only in
`gpu-handoff-provenance-selftest`. Run it directly before pushing:

```bash
python3 -B tools/scripts/test_gpu_handoff_provenance.py -k published_receipt
```

**A pin makes two separable claims, and only one is enforced everywhere.**
Provenance is "the pinned revision is an ancestor of HEAD and still carries the
named blob and tree" — a fact about history that no later commit can falsify.
Currency is "the path at HEAD still holds that same blob" — a fact any commit
touching the path invalidates. One `require_current` switch selects between
them, and it has to be honored at all three layers a caller can enter through:
`validate_handoff_routing()` in `gpu_recipe_catalog.py`, and
`validate_with_catalog()` plus `resolve_identity()` in
`gpu_handoff_provenance.py`. The resolver is the easy one to miss, because it
reads as a lookup rather than a check — and a resolver that refuses stale-at-HEAD
input makes the strict answer leak back into every consumer above it.

Currency is off by default because the required per-commit gate validates every
pinned row, not just the rows a branch touched. Asserting currency there means
one commit landing on a pinned path turns that gate red for every other PR in
flight until each repins, which serializes concurrent work across the whole
pinned set. What catches a stale pin instead is
`gpu_handoff_provenance.py check` and the diff-scoped freshness guard, which
fires only for the branch that actually moved a pinned path and prints the
repair command with it.

## The "Vellum freeze" CI job runs two checks, and the second is the one that fails

`.github/workflows/vellum-freeze-check.yml` runs `vellum_freeze_check.py` **and**
`vellum_expansion_watch_check.py` under the single job name `Vellum freeze`. A
`core/view/**` change routinely passes the first ("No transferred Vellum slice or
authority transition is affected") and fails the second, so reading the job name
sends you to the wrong script. Open the log and find which one raised.

Three non-obvious rules of the expansion-watch checker, none derivable from a
skim of the source:

- **Both `--base` and `--head` must be full 40-char SHAs.** A ref name fails with
  `base: expected full commit SHA`, which reads like a different bug than the one
  CI hit. Always `--base $(git rev-parse ...) --head $(git rev-parse HEAD)`.
- **Coverage is exact set equality**, not a superset test: the checker raises on
  `covered != affected`, so claiming an extra capability family fails exactly as
  hard as omitting one. Claiming the same family from two event files in one diff
  is also rejected (`duplicate capability-family claims`).
- **Verify a branch against its MERGE-BASE, not `origin/main`.** Diffing a branch
  that is behind main reports every watch event main has added since as a
  deletion, and the checker rejects it with
  `<event>.json: watch events are append-only`. That is an artifact of the
  comparison base, not a real append-only violation — `git merge-base origin/main
  HEAD` makes it disappear. CI compares against the PR base, so a branch that is
  merely stale never sees this.

The event itself is an append-only JSON file directly under
`.github/vellum-expansion-watch-events/`, named exactly `<event_id>.json`, with
`capability_families` sorted and drawn from the known scope set. A change that
adds no authority carries `"disposition": "watch-only-no-authority"` and
`"authority_effect": "none"`.

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

## Regenerate against the MERGED tree, not against the base you branched from

`write` derives every identity from one source commit, so a ledger regenerated
on a branch whose base has moved pins revisions the merged tree no longer agrees
with. The symptom is not a clear staleness report: it arrives as unrelated-looking
reds — three `test_gpu_recipe_catalog` failures and a
`test_gpu_handoff_provenance::test_check_reports_a_clean_ledger` assertion —
which read as a regression in the change set rather than as a base mismatch.

The order that works is the one the `ci` skill already prescribes for a pinned
path: land the file edits, bring the branch onto the merged tree, and only then
run `gpu_handoff_provenance.py write --receipt`. Regenerating before the rebase
means doing it twice.

**The regenerated ledger survives its own commit.** After committing the YAML
and receipt, `check` still reports `OK: every pinned identity matches` at the new
source commit, because the pin-refresh commit touches only paths the inventory
excludes. So the cascade above terminates after exactly one round — a second
regeneration is not needed, and running one only produces an empty diff.

## The watch-family selectors match PATHS, so a one-line include can demand an event

`tools/scripts/vellum_expansion_watch_check.py` decides which capability
families a change touches by globbing the changed path list against
`EXPECTED_SCOPES`. Nothing in that decision reads the diff. So adding
`#include <array>` to `test/test_browser_capture_tree.cpp` — a portability fix
that changes no capture behavior at all — matches `test/test_browser_capture*`
and makes `chromium-authoring-frontend` an affected family, which the trusted
base executor then requires a watch event to cover.

Two things make this expensive to find late:

- The failing check is **`Trusted base executor`**, which is not one of the five
  contexts branch protection requires, so a PR can sit `blocked` with that red
  while every required context is green and nothing names the cause.
- Its log buries the one useful line, `watch event family coverage differs;
  affected=[...] covered=[]`, under a full `Updating files:` checkout trace.

Reproduce it locally before pushing, and note that both arguments must be full
40-character SHAs — a ref name fails with `base: expected full commit SHA`,
which reads like a broken invocation rather than a real answer:

```bash
python3 tools/scripts/vellum_expansion_watch_check.py \
  --repo . --base "$(git rev-parse origin/main)" --head "$(git rev-parse HEAD)"
```

The event is a new JSON file directly under
`.github/vellum-expansion-watch-events/`, named exactly for its `event_id`,
claiming the affected families sorted. Coverage is compared for **equality**,
not containment: claiming a family the diff does not touch fails the same way
omitting one does.
