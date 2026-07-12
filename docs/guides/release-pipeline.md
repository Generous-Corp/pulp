# Release pipeline

This doc explains end-to-end how a PR merge becomes a published SDK release —
from the first commit on `main` to 14 downloadable assets on the Releases page
(including the Intel `darwin-x64` pair, cross-compiled on Apple Silicon — see
the assets section).

If you're hunting a specific layer:

- **Tag creation logic** → `auto-release.yml`, plus [versioning.md](versioning.md) for the version-bump gates.
- **Per-platform builds** → `release-cli.yml`.
- **Signing + notarization** → `sign-and-release.yml`.
- **Failure detection** → [release-watchdog.md](release-watchdog.md).

This doc is the map; those docs are the territory.

## End-to-end flow

```
PR merge to main
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│ 1. auto-release.yml                                      │
│    .github/workflows/auto-release.yml                    │
│    Trigger: push to main                                 │
│                                                          │
│    a. Diff CMakeLists.txt `project(Pulp VERSION X.Y.Z)`  │
│       between the previous push and HEAD.                │
│    b. If VERSION moved, create signed tag `vX.Y.Z`       │
│       pointing at the same commit and push it (using a   │
│       PAT because the default GITHUB_TOKEN cannot        │
│       trigger workflows from its own pushes).            │
│    c. If a `Release: skip reason="..."` trailer is on    │
│       the tip commit, suppress tag creation.             │
│    d. If the subject is `Revert "..."`, suppress.        │
│    e. Concurrency group `auto-release` ensures only one  │
│       tag-creation runs at a time.                       │
└─────────────────────────────────────────────────────────┘
     │
     │  (tag push: refs/tags/vX.Y.Z)
     │  Two workflows trigger off the same tag push — but only ONE of
     │  them can affect the release.
     │
     ├────────────────────────────┬────────────────────────────────┐
     ▼                            ▼
┌──────────────────┐   ┌──────────────────────┐
│ 2a.              │   │ 2b.                  │
│  release-cli.yml │   │  sign-and-release.yml│
│  on: push tag v* │   │  on: push tag v*     │
│  OWNS THE RELEASE│   │  cannot touch it     │
└──────────────────┘   └──────────────────────┘
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│ 2a. release-cli.yml — builds AND publishes               │
│                                                          │
│ Matrix (6 platforms, parallel):                          │
│   - darwin-arm64    → resolved release macOS runner      │
│   - darwin-x64      → cross-compiled on Apple Silicon    │
│   - linux-x64       → github-hosted ubuntu-latest        │
│   - linux-arm64     → github-hosted ubuntu-22.04-arm     │
│   - windows-x64     → github-hosted windows-latest       │
│   - windows-arm64   → github-hosted windows-11-arm       │
│                                                          │
│ For each platform: configure → build → strip → fix       │
│ rpaths → package `pulp-${PLATFORM}.tar.gz` → repackage   │
│ the SDK as `pulp-sdk-${PLATFORM}.tar.gz` → attest →      │
│ upload as an Actions artifact. The `smoke-cli` matrix    │
│ then extracts each tarball on a fresh runner and runs    │
│ `pulp help`, `pulp-cpp help`, `pulp-mcp --version` to    │
│ catch missing-symbol / bad-rpath bugs before publish.    │
│                                                          │
│ Final `release` job — the SOLE writer of the release:    │
│   1. Download all 12 matrix artifacts.                   │
│   2. Compose the body (compose_release_notes.py).        │
│   3. Generate + attest `appcast.xml`. The Sparkle feed   │
│      is a pure function of the tag name and the date, so │
│      it is written HERE. Nothing about the release       │
│      depends on macOS signing.                           │
│   4. Create the release as a DRAFT and attach all 13     │
│      assets. (A published release is immutable, so       │
│      assets can only land while it is still a draft.)    │
│   5. Verify the EXACT required asset set, generate and   │
│      upload SHA256SUMS.                                  │
│   6. Publish — claiming /releases/latest only if this    │
│      tag is the greatest published SemVer.               │
│                                                          │
│ Steps 4-6 are one job, so the draft exists for seconds   │
│ and is never observable from outside. `needs:` is        │
│ [build-cli, smoke-cli] ONLY — the advisory               │
│ universal-arch-gate is deliberately absent (see below).  │
└─────────────────────────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│ 2b. sign-and-release.yml (parallel, and inert)           │
│                                                          │
│   - Builds, code-signs, notarizes and staples the macOS  │
│     example plugin .pkg bundles, and keeps them as       │
│     workflow artifacts for debugging (pulp #1737 keeps   │
│     them off the user-facing release page).              │
│   - Holds `contents: read`. It CANNOT write to a         │
│     release. That is the point: it may fail, hang, or be │
│     cancelled without affecting whether the SDK ships.   │
└─────────────────────────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────────────────────┐
│ 3. release-reconcile.yml (every 30 min)                  │
│                                                          │
│   The only failure-handling workflow, and the only one   │
│   that can FIX anything. For each SDK tag in the last    │
│   72h it compares desired state (a published release)    │
│   against actual state and re-dispatches release-cli for │
│   any tag that is stuck, up to 3 attempts.               │
│                                                          │
│   - A tag with a live release-cli run is LEFT ALONE, at  │
│     ANY age. Slow is not broken.                         │
│   - It never cancels a run and never deletes a release.  │
│   - Tags that exhaust the retry budget get ONE incident  │
│     issue, updated in place, which closes itself on      │
│     recovery.                                            │
└─────────────────────────────────────────────────────────┘
```

## Why publication lives in one job

Publication used to be a handshake across three workflows: `release-cli`
created a draft, `sign-and-release` polled up to 50 minutes for that draft so it
could attach `appcast.xml`, and `release-publish.yml` flipped the draft to
published only once **both** legs reported success.

Every part of that was load-bearing, and the whole thing was only as reliable as
its worst leg. In July 2026 it produced a **7% first-attempt release success
rate**: 11 of 18 consecutive tags never published, several with all six platform
binaries built green. Three separate mechanisms conspired:

1. **The 50-minute poll was a time bomb.** `release-cli` does not create the
   draft until its full build matrix finishes, which really takes 70–165+
   minutes (its macOS legs queue for hours on the shared GitHub-hosted pool). So
   on every slow release, `sign-and-release` timed out — and the coordinator
   then withheld a release whose binaries were all present and correct.
2. **A "supersede reaper" destroyed in-flight releases.** `auto-release.yml`
   cancelled any release run, and deleted any draft, whose tag was older than
   the latest published release. Because the pipeline (70–165+ min) outlasts the
   gap between tags (~100 min), releases routinely complete **out of order** —
   so "older SemVer" did not mean "obsolete", and the reaper reaped healthy work.
3. **An advisory gate still blocked.** `universal-arch-gate` carried
   `continue-on-error: true`, which makes a job's *result* advisory but does
   **not** remove it from the dependency graph. The release job kept waiting on
   its 2–7 hour hosted-runner queue.

The fixes are structural rather than conventional, so they cannot quietly
regress:

- `sign-and-release.yml` holds `contents: read` — it *cannot* write a release.
- `auto-release.yml` has no `actions` scope — it *cannot* cancel a run.
- `universal-arch-gate` is not in `release`'s `needs` — it *cannot* block.
- `release-reconcile.yml` has no delete or cancel path — recovery can only ever
  drive a release *forward*.

The governing rule, learned the hard way: **slow is survivable, racy is not.** A
three-hour release still publishes. A release that something else is allowed to
cancel does not.

## The release assets (14 base)

A successful release publishes **14 base assets** to the GitHub Release page,
including the Intel `darwin-x64` CLI+SDK pair, which is now a REQUIRED leg
(cross-compiled on Apple Silicon — see the note below the table).

| Asset | Purpose |
|-------|---------|
| `appcast.xml` | Sparkle auto-update feed; consumed by `pulp upgrade --check-only` |
| `pulp-darwin-arm64.tar.gz` | CLI tarball (`pulp`, `pulp-cpp`, `pulp-mcp`, and runtime library) |
| `pulp-darwin-x64.tar.gz` | Intel (x86_64) CLI tarball — cross-compiled on Apple Silicon |
| `pulp-linux-arm64.tar.gz` | " |
| `pulp-linux-x64.tar.gz` | " |
| `pulp-windows-arm64.zip` | " |
| `pulp-windows-x64.zip` | " |
| `pulp-sdk-darwin-arm64.tar.gz` | SDK tarball (libpulp-*.a + headers + cmake helpers, for plugin authors) |
| `pulp-sdk-darwin-x64.tar.gz` | Intel (x86_64) SDK tarball |
| `pulp-sdk-linux-arm64.tar.gz` | " |
| `pulp-sdk-linux-x64.tar.gz` | " |
| `pulp-sdk-windows-arm64.tar.gz` | " |
| `pulp-sdk-windows-x64.tar.gz` | " |
| `SHA256SUMS` | SHA-256 manifest for every user-facing release asset above (13 base) |

**Intel `darwin-x64` — cross-compiled, required.** `release-cli.yml`'s
`darwin-x64` build+smoke legs (`os: macos-15-xcompile`) cross-compile the x86_64
slice on the healthy Apple-Silicon runner via `-DCMAKE_OSX_ARCHITECTURES=x86_64`
+ `-DPULP_RUST_CLI_TARGET=x86_64-apple-darwin`, NOT the flaky native
`macos-15-intel` runner (which CPU-pegs, blows any timeout, and whose timeout
*cancellation* — not absorbed by `continue-on-error` — used to skip the whole
release). The pair is REQUIRED: `release-cli.yml`'s finalizer lists it
unconditionally in `required_assets`, so a regression that drops Intel fails the
`--exact-required` check instead of silently shipping an Intel-less release. See
`docs/guides/intel-support.md`.

Anything less than the 14 base assets means part of the pipeline didn't reach the
end. Triage by which assets are present:

- **No release at all, or a release missing assets**: this cannot happen on a
  published release — the finalizer verifies the EXACT required asset set before
  it publishes, so a missing asset leaves a draft rather than shipping a partial
  release. Look at the `release-cli.yml` run for the tag. `release-reconcile.yml`
  will also re-dispatch it automatically within 30 minutes.
- **No `appcast.xml`**: `release-cli.yml`'s `release` job failed before its
  "Generate appcast.xml" step. Note `sign-and-release.yml` is NOT involved — it
  can no longer write to a release at all.
- **`appcast.xml` present, but some platform tarballs missing**: at least
  one platform leg of `release-cli.yml`'s matrix failed before its
  artifact upload. Common shape: Linux + GNU-ld link order producing
  undefined `Fc*` references from `libskia.a` (see "v0.101.x SDK saga"
  below). The `release` job uploads whatever
  `actions/download-artifact` finds; failed matrix legs simply don't
  contribute artifacts.
- **All base assets present but the release stays in DRAFT**: the `release`
  job's finalizer died between creating the draft and publishing it — most
  likely in the `--exact-required` asset check or while uploading `SHA256SUMS`.
  `release-reconcile.yml` re-dispatches this automatically; the re-run is
  idempotent and will re-drive the existing draft to published.

## Verifying a published release

The verification layers answer separate questions:

- **Signed commit or tag:** GitHub verified a cryptographic signature from a
  public signing key registered to a configured maintainer or release-bot
  account. This says who signed the source ref.
- **Protected tag:** the `refs/tags/v*` ruleset blocks force-push, deletion, and
  update of release tags. This keeps a published source ref from being silently
  moved.
- **SHA-256 checksum:** the local file bytes match the checksum Pulp published
  for that release asset. This says the downloaded file did not change between
  publishing and install.
- **Immutable GitHub Release plus release asset digest:** GitHub can verify that
  the published release record and asset bytes match the immutable release
  attestation.
- **Artifact attestation:** GitHub can verify which workflow and source ref
  produced the release artifact.

For plain shell verification, download an asset plus `SHA256SUMS` from the same
versioned release and check the exact basename. Use a release created after
`SHA256SUMS` publishing landed:

```bash
# Replace with the first release created after this workflow landed.
version=0.617.0
asset=pulp-darwin-arm64.tar.gz
base="https://github.com/danielraffel/pulp/releases/download/v${version}"

curl -fsSLO "$base/$asset"
curl -fsSLO "$base/SHA256SUMS"
awk -v file="$asset" '$2 == file { print }' SHA256SUMS | shasum -a 256 -c -
```

For GitHub's immutable-release layer, use GitHub CLI:

```bash
gh release verify "v${version}" -R danielraffel/pulp
gh release verify-asset "v${version}" "$asset" -R danielraffel/pulp
```

`gh release verify` proves the release exists and is immutable. `gh release
verify-asset` proves the local artifact is an exact match for the release asset.
These are useful confidence layers, but the README still keeps the plain
`SHA256SUMS` path so users do not need GitHub CLI for checksum verification.

For build provenance, verify the artifact attestation. This is a separate claim
from checksum verification: it identifies the GitHub workflow and source ref
that produced the asset.

```bash
gh attestation verify "$asset" \
  -R danielraffel/pulp \
  --signer-workflow github.com/danielraffel/pulp/.github/workflows/release-cli.yml
```

For `appcast.xml`, use `sign-and-release.yml` as the signer workflow. The
provenance check is especially useful for traceability, but it does not by
itself prove a self-hosted runner was uncompromised.

## Release bot signing

Automation-created release refs are signed with an SSH signing key dedicated to
the release bot. The private key is stored in GitHub Actions as
`RELEASE_BOT_SSH_SIGNING_KEY`; recovery copies live outside the repo. Workflows
write the key to `$RUNNER_TEMP`, configure Git SSH signing, create signed tags or
commits, and let the runner discard the temp file after the job.

The public signing key is registered to the maintainer's GitHub account as a
signing key, and the workflow uses that account's noreply email address. That
pair is what lets GitHub mark the automation-created commit or tag as
`Verified`.

The signing secret is required only when automation is about to create a release
tag or bot-maintained commit. If it is missing, that step fails closed instead
of quietly publishing an unsigned source ref.

Independently of manual verification, `release-reconcile.yml` re-dispatches any
recent tag that is not published within its grace window, so a stuck release
normally repairs itself without anyone looking (see release-watchdog.md).

## Manual release-cli backfills

Use `workflow_dispatch` on `release-cli.yml` when a tag already exists but the
CLI/SDK assets need to be rebuilt with the current release pipeline.

- Leave `source_ref` blank for the normal old-tag backfill. The workflow checks
  out the requested `version` tag, then overlays the current `main` copies of
  release-pipeline helper files that are safe to use with older source trees.
- Set `source_ref` only when you intentionally want to build that source ref
  under the `version` label. That is not the normal backfill path.
- Leave `make_latest` false for old-tag backfills. Set it true only when
  backfilling the current newest release after the automatic tag-triggered run
  failed before publishing.

## Worked example — the v0.101.x SDK saga

May 15, 2026. Five sequential releases (v0.101.0 through v0.101.4) tagged but
none published because `CLI linux-x64` failed at link time with undefined
references to `Fc*` symbols from `libskia.a(SkFontMgr_fontconfig.o)`. Root
cause: Linux + GNU ld processes static archives left-to-right and won't
re-scan to resolve symbols once it moves past the archive.

Five rounds of fixes were needed:

| Release | Failing target | Fix |
|---------|---------------|-----|
| v0.95–v0.97 | `pulp-cli` | #1986 (inlined fontconfig-after-skia link helper) |
| v0.98–v0.101.1 | `pulp-import-design` | #2018 (extracted helper to `tools/cmake/PulpLinkFontconfig.cmake`) |
| v0.101.2 | `examples/pulp-gain/PulpGain_Standalone` | #2056 (applied helper inside `_pulp_add_standalone` in `tools/cmake/PulpUtils.cmake`) |
| v0.101.3 | `examples/view-bridge-demo/pulp-view-bridge-demo` | #2058 — tried `target_link_options(pulp-canvas INTERFACE "-lfontconfig")`, but `INTERFACE_LINK_OPTIONS` didn't reliably propagate to transitive consumers |
| v0.101.4 | same target still failing | **#2060 — recursive walk over every executable in the project tree at end-of-configure, applies helper to each**. This finally worked. |

v0.101.5 was the first release after #2060 — and the first release with all 11
assets published since v0.94.0.

The blanket fix in #2060 lives at the bottom of the root `CMakeLists.txt` and
no longer requires per-target patching:

```cmake
if(UNIX AND NOT APPLE AND NOT ANDROID)
    include("${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpLinkFontconfig.cmake")
    function(_pulp_collect_executables_recursive out_var dir)
        # walk BUILDSYSTEM_TARGETS + SUBDIRECTORIES recursively
        ...
    endfunction()
    _pulp_collect_executables_recursive(_all_executables "${CMAKE_SOURCE_DIR}")
    foreach(_exec ${_all_executables})
        pulp_link_fontconfig_after_skia(${_exec})
    endforeach()
endif()
```

## When to update this doc

Update this doc whenever you change any of these files:

- `.github/workflows/auto-release.yml`
- `.github/workflows/release-cli.yml`
- `.github/workflows/sign-and-release.yml`
- `.github/workflows/release-reconcile.yml`
- `tools/scripts/release_reconcile.py`
- `.github/workflows/auto-release-watchdog.yml`
- `.github/workflows/release-cadence-check.yml`
- `tools/scripts/package_cli.py`
- `tools/scripts/compose_release_notes.py` — writes plain-language, high-level
  release notes for non-experts. It strips the `type(scope):` jargon and PR-number
  noise from each title, surfaces only the sections a reader cares about
  (**⚠️ Breaking changes** first, then **✨ New / 🐛 Fixed / ⚡ Faster**), and folds
  everything inside-baseball (refactors, docs, deps/CI/build/test chores, churn)
  into one collapsed **🔧 Under the hood** summary — coalescing runs of
  near-identical PRs into a single line. A `Release-Note:` commit trailer overrides
  a title with a human sentence when an author wants one. The `--tier` flag
  (default `auto`) weights the body by semver bump level: a **patch** release drops
  the "Under the hood" fold to stay light; **minor/major** keep the full body.

These files form the release pipeline. The `ci` skill (`.agents/skills/ci/SKILL.md`)
maps `.github/workflows/**` so the existing skill-sync check already requires `ci`
to be updated on workflow changes — when you do that, update this doc too.

## Related docs

- [versioning.md](versioning.md) — how version bumps happen on the PR side (the inputs to `auto-release.yml`)
- [release-watchdog.md](release-watchdog.md) — what catches silent failures in this pipeline
- [getting-started.md](getting-started.md) — what end users see after a release ships
- `.agents/skills/ci/SKILL.md` — CI operations, including local validation via `shipyard pr`
- `.agents/skills/ship/SKILL.md` — what happens at publish time (signing, notarization, packaging)
