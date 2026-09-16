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

## A test-manifest line can cross a capability family

`vellum_expansion_watch_check.py` matches changed paths against per-family
selectors, and `visual-proof-harness` owns `test/cmake/view_widget_bridge_tests.cmake`
outright. Registering **any** new test target in that manifest crosses the family
— including a control-plane or audio test with no screenshot, golden, capture or
render surface anywhere in it — so the change needs its own append-only event
under `.github/vellum-expansion-watch-events/`.

The failure reads as a false positive and is not one:

```
watch event family coverage differs; affected=['visual-proof-harness'] covered=[]
```

Two things make it hard to place. The checker reports the family, never the path
that selected it, so grep the selector lists in `vellum_expansion_watch_check.py`
for each changed path rather than guessing from the family's name. And an event
file is only counted once it is **committed** — the checker reads the diff, not
the working tree, so writing the JSON and re-running reports the identical
failure and reads as a rejected event.

The event's `capability_families` must equal the affected set exactly, and its
`rationale` should name the single line that crossed the selector, so a reader
can tell a manifest registration apart from a real harness change.

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
ledger bytes. `gpu_handoff_provenance.py check` **does** catch it now: it binds
the published receipt to the ledger's bytes (sha256 + canonical paths, no git,
no currency claim), prints a `RECEIPT …` line and exits 1 when they disagree,
and says `receipt: none at …; binding not checked` out loud when there is no
receipt to bind. It used to exit 0 on exactly the state this paragraph warns
about, while the selftest that would have caught it
(`test_published_receipt_binds_the_checked_in_ledger`) is opt-in behind
`PULP_GPU_HANDOFF_REQUIRE_CURRENT=1` — deliberately, because currency at HEAD
would go red on every unrelated PR — so a "36 tests, OK" run proved nothing
about the receipt. Read the RECEIPT line, or `--json` and the `receipt.state`
field (`bound` / `stale` / `absent`); do not infer binding from a green
selftest.

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
stale row goes out unseen.

`gates.sh` and the pre-push hook DO catch a stale pin, but only the cheap half
of it. A diff-scoped `gpu-handoff pin freshness` guard fails when a changed file
is pinned and the ledger was not touched, and it names the repair command. It
deliberately does not re-verify the identity fields, because that costs a `git
log` per pinned path. So `gates: ✓ all gates pass` proves the ledger was
*refreshed*, never that its 100+ identities are *correct* — only
`gpu_handoff_provenance.py check` proves that, and it is not run by any gate.
Run it yourself after every refresh.

**The cascade can start from a gate you were not thinking about.** A fix in
`core/` that touches a skill-mapped source path makes `skill_sync_check.py`
demand a SKILL.md edit; a SKILL.md is frequently a pinned path, so satisfying
skill-sync stales a handoff row; and the refresh commit touches the YAML, which
is itself mapped to *this* skill and so re-arms skill-sync. Landing a one-line
source fix can therefore require touching two skills and the ledger. The way
out is not to keep chasing it: satisfy skill-sync with a real gotcha where you
genuinely learned one and the `Skill-Update: skip skill=<name> reason="..."`
trailer where you did not, then refresh the ledger LAST, in its own commit.

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

**A capability-registry change stales a handoff pin without touching any GPU or
Vellum file.** `tools/scripts/test_release_artifact_contents.py` is a pinned
path *and* one of the sites that hardcodes the control-registry digest, so
adding a row to `inspect/include/pulp/inspect/capability_definitions.inc`
re-pins the digest there and stales that row by pure transitivity. The
resulting `gpu-recipe-catalog-selftest` failure names a GPU catalog and a
release-artifact script, and nothing in either message mentions the capability
registry — so the natural reading is that the row belongs to another author's
change. Before dismissing it, check whether the flagged path is in your own
diff (`git diff --name-only <base>..HEAD -- <path>`) with a control grep that
must return non-zero; the answer is frequently yes.

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

## Merging `origin/main` preserves the pin; rebasing onto it orphans it

A receipt names one `source_commit`, and `check` requires that commit to be an
ancestor of HEAD. A **merge** of `origin/main` keeps the pinned commit in the
history, so the receipt stays valid and needs no repair. A **rebase**, an
`--amend`, or a squash rewrites it, and the receipt now names a commit that no
longer exists on the branch.

The repair that suggests itself is the one that cannot converge: regenerating
writes the commit the regeneration is about to create, which is not an ancestor
of HEAD either, so the next `check` fails the same way. Record a commit that
**already exists** — the newest commit reachable from HEAD that touched a pinned
path. `resolve_source_commit` names that commit in the failure text, so the
error is the answer rather than the start of a search:

```
source commit <sha> is not an ancestor of HEAD; identities generated from it
cannot satisfy the handoff validator. A rebase, amend, or squash of the pinned
commit is the usual cause; merging origin/main preserves the pin where rebasing
onto it does not. Record a commit that already exists rather than the one
regeneration is about to create, such as <sha>
```

Two mechanics that cost time on the way to that error:

- **`check | tail` reports the pipeline's status, not the checker's.** A run
  that prints `exit=0` under a pipe may have exited 1. Read `${PIPESTATUS[0]}`,
  or drop the pipe.
- **`write --source-commit` refuses on an unclean canonical path** (rc=2). So a
  repair cannot precede the merge commit that resolves the conflict: let the
  merge land, then regenerate from the merge sha. On a checkout without the
  merge driver below, "let the merge land" means taking `--theirs` on the
  generated ledgers by hand first.

A receipt conflict is also not always pointer churn. One case reported
`102 rows unchanged` while `handoff_sha256` moved to a value matching **neither**
parent — the ledger bytes were equal and the receipt hash was not, which means
the hash was computed over a tree that no longer existed. Regenerate from the
merge sha rather than picking a side.

## `regenerate-me` in a ledger is a resolved merge, not corruption

`.gitattributes` routes `docs/status/gpu-vellum-handoff.yaml` and
`docs/validation/gpu-handoff-provenance/receipt.json` to the
`pulp-gpu-ledger` merge driver, which `setup.sh` registers via
`tools/scripts/install-githooks.sh`. The driver reads all three sides — base,
ours and theirs — and overwrites, in each of them, every Pulp row's `revision`
and `object_id`, plus the receipt's `source_commit` and `handoff_sha256`, with
the literal `regenerate-me`. Those are exactly the fields `write` regenerates,
so poisoning them makes the re-pin churn byte-identical on every side, and an
ordinary three-way merge then runs over the result.

That split is the point. Identity churn cancels out; everything else — a row
one side added, a comment somebody re-bound — is content, and merges the way
content does. `vellum_paths` rows are constants pinned to a fixed foreign
revision and are never touched. When a re-pin was the only difference the merge
completes with no conflict markers and commits itself; when two authors edited
the same row it still comes back as conflict markers and a nonzero exit, which
is what you want.

**The value is invalid on purpose, and that is the entire mechanism.** Every
resolution that produces something *shaped* like an identity is accepted
somewhere: a stale-but-ancestral pin satisfies the always-on provenance tier, so
a driver that computed the merged value — or `merge=ours` — would hand Git a
wrong answer and Git would commit it without a word. `regenerate-me` cannot
survive. It fails the 40-hex check in `validate_handoff` *and* the blob
comparison in `validate_handoff_routing`, and
`tools/scripts/gpu_ledger_sentinel_check.py` rejects it from both the pre-push
hook and `gates.sh` before it can reach CI.

So when a ledger reads `regenerate-me`, nothing is corrupt: the merge is done
and the regeneration is owed. Regenerating is not optional politeness — the
identities are invalid until you do, and every tier says so:

```sh
python3 tools/scripts/gpu_handoff_provenance.py resolve   # preferred: decides the branch below
```

`resolve` runs the whole sequence from a committed merge: it regenerates pinned
to `HEAD` — a commit that already exists, because the receipt names its own
source commit and pinning to the commit the write is about to create cannot
converge — then decides whether anything actually moved, and proves the receipt
binds the ledger it was written beside with a control on a mutated ledger that
must come back False.

**The baseline is HEAD, not `origin/main`.** Regeneration always rewrites the
receipt's `source_commit`, so a diff is never by itself evidence of movement;
and a branch that already re-pinned its ledger differs from main *for a reason
that is not movement*, so taking main as the baseline calls an inert merge a
re-pin and commits the churn it was supposed to prevent. The question is only
whether regeneration changed what HEAD committed.

| Signal | Verdict | What `resolve` does |
|---|---|---|
| regeneration changes the ledger HEAD committed | `MOVED` | keeps it; reports `repaired N identity fields` |
| ledger unchanged, HEAD's receipt already binds it | `CHURN` | keeps HEAD's bytes; writes and commits nothing |
| ledger unchanged, HEAD's receipt does not bind it | `REBIND` | rewrites the receipt only — a text merge that took one side of the pair |
| ledger unchanged, receipt would change in a field that cannot move on an unmoved ledger | refuses (exit 3) | that is a human edit |

It also refuses (exit 2) while `MERGE_HEAD` is present or the index holds
unmerged entries, on an unclean canonical path, and (exit 3) when
`regenerate-me` survives regeneration — which means the driver poisoned a field
`write` does not rewrite, and no repair command clears it. Add `--commit` to
land the result, and `--json` for the verdict as data.

The manual form remains available and is what `resolve` performs:

```sh
python3 tools/scripts/gpu_handoff_provenance.py write --source-commit HEAD --receipt
git commit docs/status/gpu-vellum-handoff.yaml \
           docs/validation/gpu-handoff-provenance/receipt.json
```

Land that as **its own commit**: amending a commit that touches a pinned path
changes that path's owning revision and re-stales the row just repaired. Drop
`--receipt` and the ledger is repaired while the receipt stays bound to bytes
that no longer exist — green locally, red in CI.

**`check` answers the binding question, and it is the only thing that does.**
The identity tiers compare pins against Git; none of them reads the receipt, so
every pinned identity can match while the receipt names a ledger that no longer
exists. `check` therefore compares `sha256(ledger)` against the receipt's
`handoff_sha256` unconditionally — not behind a flag — and prints
`RECEIPT …` plus a nonzero exit when they disagree, or `the receipt binds this
ledger` when they agree. When no receipt is present it says so loudly rather
than exiting 0 on a claim it never examined.

Two limits worth knowing before trusting the driver:

- **A checkout that never ran `setup.sh` has no driver**, because Git resolves
  `merge=<name>` against *local* config that no clone carries — and it does not
  error on a name it cannot resolve, it falls back to the ordinary text merge in
  silence. A checkout bootstrapped *before* the driver landed is the same state
  and the likelier one: the attribute is there, so the automation looks
  installed while every sweep re-conflicts. `gpu_ledger_sentinel_check.py` now
  reports that directly — it reads the routed paths out of `.gitattributes` and
  asks Git whether the name resolves — so the pre-push hook and `gates.sh` both
  fail with `install-githooks.sh` as the repair. Re-running the installer is
  idempotent and takes a second.
- **GitHub's server-side merge does not run merge drivers.** A pull request can
  still show `CONFLICTING` on github.com while the same merge is clean locally.
  Merge `origin/main` into the branch, regenerate, and push.

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
