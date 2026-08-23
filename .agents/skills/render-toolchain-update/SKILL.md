---
name: render-toolchain-update
description: |
  Update Pulp's pinned Skia, Dawn, and optional V8 prebuilts as one milestone-matched
  render-toolchain release. Use for requests such as "update Skia/Dawn", "move to
  chrome/mNNN", "find the matching V8", "update the GPU toolchain", or "use the
  Skia/Dawn/V8 tuple that goes together". Distinguishes milestone-matched V8 releases
  from v8-builder's newer weekly LKGR releases, verifies exact upstream provenance and
  asset hashes, updates every Pulp mirror, and runs the provider-identity/ODR gates.
---

# Update the matched render toolchain

This procedure expects `ghapp` and `python3`, and operates on
`tools/deps/manifest.json`, `tools/scripts/fetch_skia_for_release.py`, and
`tools/scripts/fetch_v8_for_release.py`.

Pulp's default is a milestone-matched set: the published `skia-builder` release,
the Dawn revision that Skia itself built from, and the V8 revision from the Chromium
milestone branch whose DEPS pins that exact Skia commit. `v8-builder` may also publish
newer weekly LKGR releases; those remain valid opt-in V8 choices, but they are not the
default pin for a milestone update.

## Truth model

Keep these values distinct in notes and manifests:

- `skia`: exact commit at `refs/heads/chrome/mNNN` used by skia-builder.
- `built_dawn`: Dawn from that Skia commit's own `DEPS`; this is the Dawn actually
  compiled into the Skia/Dawn artifacts.
- `v8`: V8 from Chromium `refs/branch-heads/<branch>` DEPS for milestone NNN, after
  asserting that Chromium's `skia_revision` equals the published Skia commit.
- `dawn`: Chromium's Dawn pin. It can differ from `built_dawn`; record the mismatch.
  Do not claim all three are one identical Chromium DEPS tuple when it differs.

`v8-builder/tools/milestone_pin.py` resolves and enforces this contract. For example:

```bash
python3 ../v8-builder/tools/milestone_pin.py 152 \
  --skia-release-tag chrome/m152 > /tmp/m152-render-lock.json
python3 -m json.tool /tmp/m152-render-lock.json
```

The result must have the requested `milestone`, `skia_release_tag`, exact `skia` and
`v8` SHAs, and `built_dawn`. A false `dawn_matches_chromium` is disclosed provenance,
not a failure: Skia is built against its own Dawn pin.

## Release lanes

- `skia-builder` publishes `chrome/mNNN`, then dispatches
  `skia_milestone_published` to v8-builder.
- v8-builder's `matched-milestone.yml` resolves the lock, skips an exact release that
  already exists, and dispatches the sealed all-platform build with publication on.
- v8-builder's `release-watch.yml` continues its weekly LKGR cadence independently.

For a historical Skia release or recovery, manually start the same idempotent lane:

```bash
ghapp workflow run matched-milestone.yml \
  --repo danielraffel/v8-builder \
  -f milestone=152 \
  -f skia_release_tag=chrome/m152
```

Do not update Pulp's V8 pin until that matched release is published and every requested
platform asset is present. Never substitute the newest weekly V8 merely because it is
newer.

## Matched release collection

Present the Skia/Dawn and V8 release URLs together as the milestone collection, then
download only the assets the user needs:

```bash
ghapp release view chrome/m152 --repo danielraffel/skia-builder --json url
ghapp release list --repo danielraffel/v8-builder --limit 100 --json tagName \
  --jq '.[] | select(.tagName | startswith("v8-m152-")) |
    "https://github.com/danielraffel/v8-builder/releases/tag/\(.tagName)"'
```

The collection is a provenance and compatibility convenience, not one combined archive:
Skia/Dawn, V8, or both may be consumed independently.

## Pulp update checklist

1. Work from current `origin/main` in a clean worktree. Read release notes from M+1
   through the target and search Pulp for removed APIs.
2. Inspect the published Skia release with `ghapp release view chrome/mNNN --repo
   danielraffel/skia-builder --json assets,publishedAt`. Update every platform URL and
   SHA-256 in `tools/deps/manifest.json`; do not omit Windows, Linux ARM64, Apple device
   and simulator slices, WASM, or XCFramework coverage.
3. Inspect one native archive and `external/skia-build/VERSION.md`. Confirm the exact
   Skia and built-Dawn SHAs, deployment floors, and optional archives such as Skottie,
   `jsonreader`, and `skresources`.
4. Inspect the matched v8-builder release manifests. All assets must agree on
   `pair.pair_kind=chromium-milestone`, `pair.milestone`, `pair.skia`, `pair.v8`,
   `pair.built_dawn`, and `pair.validated_skia_release`.
5. Update the V8 entry, all asset URLs/hashes, `DEPENDENCIES.md`, V8 provider comments,
   `tools/cmake/FindV8.cmake`, `tools/cmake/PulpV8Windows.cmake`, test fixtures, and
   `tools/deps/min_os.json` only from measured release facts.
6. Update the Skia/Dawn mirrors: `external/skia-build/VERSION.md`, visual-harness pins,
   build-script defaults, docs/support matrix, and manifest fixtures.
7. Run the manifest mirror/audit tests and both fetch-script suites. Fetch a real native
   Skia asset and matched V8 asset, configure with GPU + Lottie + V8, and run the
   provider-identity/ODR validation. A pixel-only test is insufficient.
8. Measure every Apple slice actually selected by the manifest. A same-tag asset can
   leak a higher deployment target than its universal sibling; use the verified
   universal slice or rebuild rather than publishing a false minimum-OS claim.
9. Re-bake CI goldens when either prebuilt pin changes, then ship only after required CI
   and cross-platform asset coverage are green.
10. Exercise the shared-cache publication path with
    `--cache-lock-timeout`: a pin-stale or cold cache must be populated only by
    the lock owner in a private sibling staging directory, then renamed into an
    immutable platform-plus-asset-SHA generation. A waiter must recheck the
    winner's exact stamp plus platform library before it skips downloading, and
    a pin bump must preserve the prior generation for bound consumers. Never
    seed the canonical cache by rsyncing a checkout merely because
    `external/skia-build/build` exists. Keep release x64/universal destinations
    isolated from the host arm64 cache.

## Common traps

- A Skia milestone name alone does not select a V8 revision. Resolve through Chromium's
  milestone branch and assert the exact Skia SHA.
- Skia's Dawn pin and Chromium's Dawn pin are separate dependency surfaces.
- GitHub release tags containing `/` must remain correctly URL-encoded/handled.
- Linux x64 Skia and V8 assets must retain the portable glibc floor; do not replace
  their portable releases with a normal ubuntu-latest artifact.
- `fetch_skia_for_release.py` platform keys must match the manifest exactly (notably
  `wasm-wasm32`).
- Keep release-fetch progress output ASCII-safe. Windows release runners can use a
  cp1252 console, where decorative Unicode arrows raise `UnicodeEncodeError` before
  an asset download starts; exercise the full Windows fetch path with cp1252 stdout.
- JS-engine wording in `tools/deps/manifest.json`, `tools/deps/min_os.json`, and
  `tools/cmake/FindV8.cmake` describes a *selection contract*, not just prose.
  The contract is: `auto`/`quickjs` compile QuickJS only; `jsc` additionally
  compiles `core/view/src/js_jsc_engine.mm` and links
  `JavaScriptCore.framework` on Apple; `v8` selects the sealed prebuilt. JSC is
  **opt-in**, never implied by "Apple". Older text said "default is QuickJS, JSC
  on Apple", which reads as JSC being automatic on Apple platforms and is wrong.
  Likewise iOS is no longer "JSC-only": V8 is excluded there because JIT is
  forbidden, but QuickJS is the default and JSC stays opt-in. When a pin or
  min-OS note is edited, keep these three files saying the same thing — they are
  the only place the engine contract is written down outside the CMake modules.
