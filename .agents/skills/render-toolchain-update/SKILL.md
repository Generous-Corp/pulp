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

Pulp's default is a milestone-matched, provider-validated set: the published
`skia-builder` release, the Dawn revision that Skia itself built from, and the V8
revision from the same Chromium milestone branch. Chromium's raw Skia/Dawn DEPS pins
may differ from the later published Skia branch head; that is valid only when the
v8-builder release records both generations, binds `built_skia`/`built_dawn` to the
published provider, and passes the combined provider-identity/ODR gate. `v8-builder` may also publish
newer weekly LKGR releases; those remain valid opt-in V8 choices, but they are not the
default pin for a milestone update.

## Truth model

Keep these values distinct in notes and manifests:

- `skia`: exact commit at `refs/heads/chrome/mNNN` used by skia-builder.
- `built_dawn`: Dawn from that Skia commit's own `DEPS`; this is the Dawn actually
  compiled into the Skia/Dawn artifacts.
- `v8`: V8 from Chromium `refs/branch-heads/<branch>` DEPS for milestone NNN, after
  recording Chromium's raw Skia revision separately from the provider generation
  validated by the matched release.
- `dawn`: Chromium's Dawn pin. It can differ from `built_dawn`; record the mismatch.
  Do not claim all three are one identical Chromium DEPS tuple when it differs.

`v8-builder/tools/milestone_pin.py` resolves and enforces this contract. For example:

```bash
python3 ../v8-builder/tools/milestone_pin.py 153 \
  --skia-release-tag chrome/m153 > /tmp/m153-render-lock.json
python3 -m json.tool /tmp/m153-render-lock.json
```

The result must have the requested `milestone`, `skia_release_tag`, exact Chromium
`skia`/`v8`/`dawn` SHAs, and exact `built_skia`/`built_dawn` provider SHAs. False
`skia_matches_chromium` or `dawn_matches_chromium` values are disclosed provenance,
not failures: acceptance instead requires `validated_skia_release` to equal the
active Skia version, `built_skia`/`built_dawn` to equal Pulp's active provider, every
asset's embedded pair to agree, and the combined provider-identity/ODR gate to pass.

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
  -f milestone=153 \
  -f skia_release_tag=chrome/m153
```

Do not update Pulp's V8 pin until that matched release is published and every requested
platform asset is present. Never substitute the newest weekly V8 merely because it is
newer.

## Matched release collection

Present the Skia/Dawn and V8 release URLs together as the milestone collection, then
download only the assets the user needs:

```bash
ghapp release view chrome/m153 --repo danielraffel/skia-builder --json url
ghapp release list --repo danielraffel/v8-builder --limit 100 --json tagName \
  --jq '.[] | select(.tagName | startswith("v8-m153-")) |
    "https://github.com/danielraffel/v8-builder/releases/tag/\(.tagName)"'
```

The collection is a provenance and compatibility convenience, not one combined archive:
Skia/Dawn, V8, or both may be consumed independently.

## Independent Skia/Dawn advance

When the user explicitly asks to advance Skia/Dawn before the matching V8 build
is complete, keep the lanes separate instead of blocking the render update or
claiming a matched tuple:

1. Update the active Skia entry and its built-Dawn provenance from published
   assets. Leave the V8 version, assets, and its internal `paired_*` metadata at
   their last verified milestone.
2. State the mixed active-provider selection in `DEPENDENCIES.md` and the V8
   manifest notes. Do not rewrite V8's historical `skia_release_tag`,
   `paired_skia`, or `paired_dawn` to the newer active Skia values.
3. Run the sealed V8 provider-identity/ODR gate against the new Skia/Dawn
   provider. Keep this in the required release-path CI surface while the mixed
   selection is active; a successful one-off local or Skia-only build does not
   prove mixed-provider safety.
4. Record a precise follow-up trigger: adopt V8 only after the matched release
   contains every required platform asset and its embedded pair manifest passes
   the normal milestone checks.

This is a bounded compatibility state, not a new default release policy. Return
to a fully milestone-matched selection as soon as the verified V8 release is
available.

## Pulp update checklist

For the canonical executable validation and machine-readable result, run:

```bash
python3 tools/deps/validate_hosts.py --render-toolchain
```

The local native arm64 leg performs the full mixed-provider proof. Configured
Unix remotes populate and verify their own immutable generation, run the
capability probe, and require the second fetch to be a no-download hit. The
underlying `validate_render_update.py` JSON records platform, asset SHA,
generation receipt, capability result, cache result, and mixed-provider result;
retain those fields in the PR/landing evidence.

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
   For m153+, run `python3 tools/scripts/verify_skia_m153_capabilities.py
   --platform <matching-native-desktop-platform> --skia-dir
   <materialized-generation>`. Run each architecture on its matching host; the
   compile-and-execute probe intentionally rejects mobile, WASM, Windows, and
   cross-architecture assets. The
   probe must reject a directory unless its verified asset stamp matches that
   platform's manifest digest and both Skia and Dawn archives are materialized.
   It proves the new API and exported-symbol surface against the exact provider;
   actual Graphite executor dispatch belongs to the integration's measured
   behavior gate, not this toolchain probe.
   A `darwin-universal` provider is the deliberate aggregate exception: run it
   on darwin-arm64 so the probe compile/links/runs arm64 natively and x86_64
   explicitly through Rosetta. Its single JSON result binds both records to the
   same universal asset digest, generation receipt, probe source, and Pulp
   source SHA. A universal build/lipo check without that dual-slice receipt is
   not m153 capability evidence.
8. Measure every Apple slice actually selected by the manifest. A same-tag asset can
   leak a higher deployment target than its universal sibling. Measure the exact
   thin archive when the manifest selects a thin archive; evidence from a
   universal sibling is not interchangeable. Use the verified selected asset or
   rebuild rather than publishing a false minimum-OS claim.
9. Re-bake CI goldens when either prebuilt pin changes, then ship only after required CI
   and cross-platform asset coverage are green.
10. Exercise the shared-cache publication path with
    `--cache-lock-timeout`: a pin-stale or cold cache must be populated only by
    the lock owner in a private sibling staging directory, then renamed into an
    immutable platform-plus-asset-SHA-plus-receipt-schema generation. A waiter must recheck the
    winner's exact stamp plus platform library before it skips downloading, and
    a pin bump must preserve the prior generation for bound consumers. Never
    seed the canonical cache by rsyncing a checkout merely because
    `external/skia-build/build` exists. Keep release x64/universal destinations
    isolated from the host arm64 cache.
11. After merge, prewarm every active build host through the fetcher's normal
    immutable cache-owner path. For each M3, M5, M1, Mac mini, and Mac Pro host,
    record the exact asset SHA, materialized `libskia.a` plus
    `libdawn_combined.a`, and a second invocation that reports the complete
    generation and performs no download. Skip or defer a host only with an
    explicit offline/retired disposition; never copy a checkout cache between
    machines.
12. Treat missing provenance as a cold cache. A materialized library plus a
    tracked `VERSION.md` digest is not proof that those bytes came from that
    archive: without the exact fetcher-written asset stamp, re-download and
    verify before publication. Source fallbacks must likewise pin the builder
    revision and fail before copying output unless the built Skia checkout HEAD
    equals the manifest's immutable `skia_commit`; a milestone branch name alone
    is never sufficient provenance.

## Common traps

- A Skia milestone name alone does not select a V8 revision. Resolve through Chromium's
  milestone branch, preserve its raw Skia SHA, and separately prove the release's
  validated `built_skia`/`built_dawn` pair equals Pulp's active provider.
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
  Likewise iOS is no longer "JSC-only": Pulp has no device/AUv3 V8 runtime
  acceptance or packaging contract. The m153 V8 release includes a jitless
  simulator framework only for provider/header provenance validation; it is not
  selectable as an iOS runtime. QuickJS is the default and JSC stays opt-in.
  When a pin or min-OS note is edited, keep these three files saying the same
  thing — they are the only place the engine contract is written down outside
  the CMake modules.
- Matched V8 cold-fetch validation resolves the immutable builder tag through
  the GitHub API. CI callers must expose their read-only `${{ github.token }}`
  as `GH_TOKEN`; otherwise a valid sealed asset can fail after download when
  the unauthenticated API budget is exhausted. The fetcher deliberately sends
  that credential only to `https://api.github.com` and refuses redirects; it is
  never forwarded to release-asset or cross-origin targets.
