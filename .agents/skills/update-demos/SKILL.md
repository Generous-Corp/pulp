---
name: update-demos
description: |
  Rebuild, re-pin, and republish Pulp's downstream demo/example repos against a
  new or the latest SDK. Routes natural-language requests — "update the demos to
  the latest SDK", "rebuild the examples against the new SDK", "check the demos
  still build on the new SDK", "bump every consumer's SDK pin", "open the
  SDK-update PRs", "republish the demo packages" — to the `pulp minos`
  Managed SDK Consumer Sweep: `sweep` (build + measure min-OS floors),
  `update` (bump each repo's SDK pin, open PRs), and `publish-runbook`
  (per-repo rebuild/package/publish steps).
requires:
  tools:
    - pulp
  files:
    - tools/scripts/sdk_consumer_sweep.py
    - tools/scripts/sdk_consumer_update.py
    - planning/sdk-consumers/consumers.yaml
---

# update-demos

## When this skill applies

The user asks, in any phrasing, to move the downstream demo/example repos onto a
different Pulp SDK. Triggers include:

- "update the demos to the latest SDK"
- "rebuild the examples / demos using the latest version of the SDK"
- "do the new SDK build all the demos still?" / "check the demos still build"
- "bump every consumer's SDK pin to X.Y.Z"
- "open the SDK-update PRs for the demos"
- "republish the demo packages against the new SDK"

These are the **Managed SDK Consumer Sweep** — one registry of ~15 buildable
downstream repos, driven by `pulp minos {sweep,update,publish-runbook}`. See the
memory `managed-sdk-consumer-sweep` for the registry contents and history.

## The registry lives in the private `planning` submodule

The consumer list is `planning/sdk-consumers/consumers.yaml`. A fresh public
clone will not have it. If any `pulp minos sweep|update|publish-runbook` prints
`consumers registry not found`, initialize the submodule first:

```bash
git submodule update --init planning
```

The tools also need PyYAML (`python3 -m pip install pyyaml`) — they print a clear
install hint if it is absent.

## The four-step flow

### 1. Resolve the target SDK version

"Latest" means the newest published SDK release, not necessarily what this
checkout is at. Resolve a concrete `X.Y.Z` before doing anything:

```bash
pulp sdk available        # lists published SDK releases; newest is the target
# or, for the version this source tree builds:
pulp version              # SDK + project version of the current checkout
```

Pin the concrete version for the rest of the flow (e.g. `--to 0.640.0`). Never
pass a floating `latest` to `update` — the pins written into consumer repos must
be exact semver.

### 2. Verify every consumer still builds — `pulp minos sweep`

The sweep clones each buildable consumer, configures it against an **installed**
SDK (`find_package(Pulp)` + `CMAKE_PREFIX_PATH`), builds it, measures every
built artifact's min-OS floor, and reports build ok/fail plus floor-vs-SDK-floor
drift. Non-zero exit on any build failure or drift.

```bash
# Build the SDK and install it to a prefix the sweep can point at:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(getconf _NPROCESSORS_ONLN)"
cmake --install build --prefix /tmp/pulp-sdk

# Dry-run first — prints the plan (what will build, what is skipped) without cloning:
pulp minos sweep --sdk-prefix /tmp/pulp-sdk --dry-run

# Real run (all buildable consumers), or narrow with --only:
pulp minos sweep --sdk-prefix /tmp/pulp-sdk
pulp minos sweep --sdk-prefix /tmp/pulp-sdk --only pulp-gpu-nam --json
```

Three repos (`pulp-spectral-lab`, `pulp-superconvolver`, `pulp-tempo-sampler`)
are README/PKG-only release mirrors with no public source — the sweep skips them
by design; that is not a failure.

### 3. Bump each consumer's SDK pin and open PRs — `pulp minos update`

`update` rewrites each consumer's SDK pin (`pulp.toml` `sdk_version`,
`find_package(Pulp X.Y.Z)`, FetchContent `GIT_TAG v…`; a floating `latest` pin
is left alone). It is **dry-run by default** — you must pass `--open-prs` to
clone, edit, branch, push, and `gh pr create`.

```bash
# See exactly which pins in which repos would change:
pulp minos update --to 0.640.0

# Actually open one chore/sdk-0.640.0 PR per repo:
pulp minos update --to 0.640.0 --open-prs
```

Review the dry-run diff before `--open-prs`. Merge the resulting PRs the normal
way (each repo's own CI + review).

### 4. Republish the packaged demos — `pulp minos publish-runbook`

Packaged demos (signed PKG/DMG on public releases) are **not** auto-published —
each has its own signing identity and release process. `publish-runbook` prints
the per-repo rebuild → package → publish steps to run by hand after the pin PRs
merge:

```bash
pulp minos publish-runbook --to 0.640.0
```

## Guardrails

- **Sweep before update.** Don't bump pins the sweep hasn't proven build. A green
  sweep is the evidence that `--to X.Y.Z` won't break a consumer.
- **`update` is dry-run until `--open-prs`; `publish-runbook` never mutates.**
  Nothing pushes or publishes without an explicit flag / manual step.
- **The floor can legitimately differ per repo.** GPU NAM's heavier deps may
  floor above a simple demo — the sweep reports each repo's own floor; a floor
  *above* the SDK floor is information, only a floor *below* is drift.
- **Measuring one binary's floor** (no registry needed) is the primitive under
  all of this: `pulp minos measure <path/to/binary>` — also exposed over MCP as
  `pulp_minos`. See `docs/guides/minimum-os-support.md`.

## Related

- User guide: `docs/guides/minimum-os-support.md`
- CLI reference: `docs/reference/cli.md` (`minos`)
- Proposal / history: `planning/2026-07-07-managed-sdk-consumer-sweep-runner-proposal.md`
- Min-OS propagation into consumers: `tools/cmake/PulpMinOs.cmake`
