---
name: intel-canary
description: Maintain Pulp's macOS Intel (x86_64) portability lint and CI tiering — the PULP_INTEL_CANARY configure gate, intel_canary_lint.py + its allowlist, and the Tier 0-3 workflows (build.yml canary step, intel-portability.yml, nightly-intel.yml, release-cli.yml universal gate). Use when touching cross-arch build wiring, arch-gated SIMD, or Intel CI.
requires:
  scripts:
    - tools/scripts/intel_canary_lint.py
    - tools/scripts/intel_canary_allowlist.txt
    - tools/scripts/test_intel_canary_lint.py
  tools:
    - python3
    - yamllint
    - actionlint
---

# Intel canary skill

Pulp ships macOS arm64 as `usable` and macOS x86_64 / universal as
`experimental`. This skill owns the machinery that keeps the Intel story from
silently regressing. The full design, tiering, and the honest catch/miss list
live in `docs/guides/intel-support.md` — read it first.

## The Tier-1 Rosetta lane measures logic, not speed

`intel-portability.yml` runs x86_64 binaries under Rosetta on an arm64 runner, at
roughly a third of native speed. Anything asserting a wall-clock budget therefore
reports the emulator.

That lane went red for seven consecutive runs on exactly three of 16,314 tests,
all timing-bound: `heritage-performance` (labels `performance quality-lab`), a
`CompiledTempoMap` randomized-map **Timeout**, and `process-deadline-selftest`.
None was an x86_64 defect.

The cause was a drifted exclusion list: `build.yml` excludes
`performance|bench|quality-lab` on PR runs, this lane excluded only
`validation|slow`. When adding a timing-sensitive label or a deadline test, update
both — and remember `--repeat until-pass:2` only absorbs a *single* flake, so a
consistently slow test fails twice and reds the lane.

A red here is advisory and blocks nothing, which is precisely why it can sit red
for days while burning ~52 minutes of hosted macOS per triggering PR.

## What this skill covers

- `tools/scripts/intel_canary_lint.py` — the Tier-0 static lint (5 classes).
- `tools/scripts/intel_canary_allowlist.txt` — its exemption list.
- `tools/scripts/test_intel_canary_lint.py` — the lint's self-test.
- The `PULP_INTEL_CANARY` option in the root `CMakeLists.txt`.
- The Tier 0-4 workflows: `build.yml` (canary step), `intel-portability.yml`
  (Tier 1 advisory PR lane), `nightly-intel.yml` (Tier 2), and the
  `universal-crosscheck` job in `nightly-intel.yml` (Tier 3, nightly), plus the
  dedicated physical Intel selector and supervisor (Tier 4, advisory).

Note: the Tier-3 universal check only *validates* a universal build — it publishes
nothing, and the release ships THIN per-arch binaries. It lives in
`nightly-intel.yml`'s `universal-crosscheck` job (nightly), NOT on the release
path: a redundant canary for an artifact we do not ship must never be able to
block or starve a release. `intel-portability.yml` covers Intel at PR time.

Its `auval` step must never be written as `auval | tee /dev/stderr | grep -q PASS`.
Under `set -o pipefail`, `grep -q` exits on its first match and SIGPIPEs `tee`,
failing the step even though auval printed "AU VALIDATION SUCCEEDED" — and `grep`
for bare `PASS` matches a per-subtest line that prints even on an overall failure.
Capture to a file, then assert on `AU VALIDATION SUCCEEDED`. That false failure is
what got auval misdiagnosed as flaky ("Bad Max Frames") and as "unreliable on
hosted VMs"; it passes fine there.

The **installable** Intel artifact is a separate concern — a REQUIRED
`darwin-x64` build+smoke leg in `release-cli.yml` (`os: macos-15-xcompile`) that
ships `pulp-darwin-x64.tar.gz` + `pulp-sdk-darwin-x64.tar.gz` in every release.
It is **CROSS-COMPILED on the healthy Apple-Silicon runner** (`-DCMAKE_OSX_ARCHITECTURES=x86_64`
+ `-DPULP_RUST_CLI_TARGET=x86_64-apple-darwin`), NOT the flaky native
`macos-15-intel` image — that native leg CPU-pegged and never shipped
an artifact; its timeout *cancellation* (unabsorbed by `continue-on-error`)
turned build-cli's aggregate `cancelled` and skipped the whole release. So BOTH
"Intel is validated" (the Tier-3 gate, now advisory) and "Intel ships via
release-cli" (this cross-compiled leg) now hold. `macos-15-intel` survives only
as the Tier-2 nightly's native-silicon signal. See `docs/guides/intel-support.md`
→ "Shipped Intel artifacts".

## The five lint classes (and why they are scoped the way they are)

1. Raw NEON intrinsics / `arm_neon.h` outside an `__aarch64__`/`__ARM_NEON`
   guard — **`core/` only**.
2. An ARM-gated SIMD `#if` chain with no `__x86_64__`/`__SSE*` sibling branch
   AND no `#else` fallback — **`core/` only**.
3. Hardcoded `darwin-arm64` / `mac-arm64` / `aarch64` — **`tools/cmake/**` +
   `tools/scripts/fetch_*`**, allowlist-driven.
4. `CMAKE_SYSTEM_PROCESSOR` (host arch) used for an Apple target decision in a
   file that never consults `CMAKE_OSX_ARCHITECTURES` — **`tools/cmake/**`**,
   file-scoped.
5. Hardcoded `CMAKE_OSX_ARCHITECTURES=arm64` (arm64-only) — **`tools/cmake/**`**.

## Non-obvious gotchas (learned building this)

- **`vst3` is NOT a NEON intrinsic.** The NEON store intrinsics are
  `vst1..vst4`, so a naive `\bvst[1-4]` regex flags the VST3 plugin format, its
  `vst1`/`vst2` variants, every `#include ".../vst3_*.hpp"`, and the
  `pulp::format::vst3` namespace — 60+ false positives. Class-1/2 detection
  therefore requires the canonical NEON **lane-type suffix** (`_f32`, `_s16`,
  `_u8`, `_p64`, …) or the `vldN`/`vstN` load/store shape. If you widen the NEON
  token set, keep the suffix requirement or the VST3 tree lights up red.
- **The lint MUST stay clean on a healthy tree.** `--mode=tree` returning a
  finding is a real portability regression to fix at the source — do NOT weaken
  the lint or blanket-allowlist to make it pass. The allowlist is for
  genuinely-arch-aware occurrences ONLY, each with a stated reason.
- **Allowlist entries are `path :: substring`, not line numbers.** Substrings
  survive line moves and force each exemption to name the exact construct. A
  coarse whole-file exemption would hide a newly-added hardcoded arm asset.
- **Class 3 skips comment lines** (`^\s*#`, `^\s*//`) but Python **docstrings**
  are not `#` comments — the `fetch_*` docstrings that list `darwin-arm64,
  darwin-x64, …` are allowlisted explicitly.
- **Class 4 is file-scoped by design.** Detecting "used for an Apple target
  decision" precisely in CMake needs semantic analysis; instead it flags a
  `CMAKE_SYSTEM_PROCESSOR` read in an Apple-aware file that never mentions
  `CMAKE_OSX_ARCHITECTURES`. Correct files (they consult the target arch) are
  clean; a new file that forgets it is caught. This is documented as a `MISSES`
  limitation in the guide.
- **`macos-15-intel` is real but flaky** (linker-image errors, CPU-peg). It is
  the fallback for Tier-2 job A, which has an infra-vs-product watchdog
  classifier. The physical Tier-4 runner uses only
  `pulp-intel-native,pulp-host-macmini`; never give it the required Studio pool
  labels and never route Intel work to Namespace.
- **A universal wgpu dylib must be re-signed after `lipo`** (G3 lesson): a raw
  fat dylib fails `codesign --verify` and the arm64 slice is killed at load. The
  Tier-2/3 `check_bundle_architectures.py --strict` assertions verify BOTH
  `lipo -archs` and `codesign --verify` on every embedded dylib for this reason.
- **A JIT workspace is not a credential boundary.** The physical Tier-4 host
  keeps `gh`/`ghapp` and runner-group administration in the login-account
  controller, but executes `run.sh` and workflow steps as the non-admin
  `pulp-ci` account through a root-owned, fixed-operation worker shim. Never
  install controller credentials in the build account or make the shim/job
  checkout writable by it. Every cycle starts from a root-owned runner/tool
  golden and deletes the complete job root plus leftover account processes;
  cleaning `_work` alone permits runner persistence. Activation requires the
  fixed hidden uid-499 account, Xcode, immutable read-only warm cache, shim, and
  narrow sudoers setup documented in `docs/guides/local-ci.md`. The worker gives
  each job a private ephemeral home and temp root, then kills and removes all
  uid-owned residue before serving the next job. Its protected warm cache is
  read-only to jobs and sets `CCACHE_NODEPEND=1`.

## Editing checklist

When a new packaging contract intentionally names thin architecture artifacts,
allowlist both the architecture-selection predicate and any literal fixture
slug. Keep the entries narrow and explain why each apparent arm64 assumption is
portable; never blanket-exempt the containing Rack or packaging file.

When you change the lint, the allowlist, or an Intel workflow:

1. `python3 tools/scripts/test_intel_canary_lint.py` — self-test must pass.
2. `python3 tools/scripts/intel_canary_lint.py --mode=tree` — must be clean.
3. `yamllint --no-warnings -d relaxed .github/workflows/` and
   `actionlint -shellcheck= -pyflakes= .github/workflows/<file>` for any YAML.
4. If you add/remove a lint class or change scope, update
   `docs/guides/intel-support.md` (catch/miss list) and this skill.
5. `tools/scripts/skill_path_map.json` maps the lint + allowlist + self-test to
   this skill — a diff touching them without updating this SKILL.md is rejected
   by `skill_sync_check.py`.

### Never let a flaky advisory leg decide a run's conclusion

`nightly-intel` concluded **`cancelled` on every scheduled run** for a long time,
which reads as "this workflow produces no Intel coverage". That reading is wrong, and
the trap is worth internalising:

```
Universal + lipo + dual-arch auval (macos-15) : SUCCESS    <- every night
Native Intel build + test (macos-15-intel)    : cancelled  <- its 120m job TIMEOUT
Intel nightly watchdog                        : SUCCESS
```

`universal-crosscheck` — the arm64+Rosetta lipo + dual-arch auval signal that
`release-cli.yml` relies on after the per-tag universal gate was removed — **succeeds
every night**. The Intel signal was there the whole time, buried under a run-level
conclusion poisoned by a *different* leg.

`native-intel` on `macos-15-intel` had **never once completed**: that image CPU-pegs,
so the job hit its 120-minute limit every run. **GitHub reports a job timeout as
`cancelled`, and a cancelled job cancels the whole RUN.** So a leg producing zero
signal was deciding the conclusion of the leg producing the real one — while burning
two hours of a scarce Intel runner nightly.

**Fix pattern: bound the work in the STEP, not with the job timeout.**

```bash
if python3 tools/ci/run_with_timeout.py 4500 \
    cmake --build "$BUILD_DIR" -- -k 0 2>&1 | tee build.log; then
  echo "status=pass" >> "$GITHUB_OUTPUT"
elif [ "${PIPESTATUS[0]}" = "124" ]; then     # WE killed it, not GitHub
  echo "::warning::runner pegged — INFRA, not a product failure"
  echo "status=infra-timeout" >> "$GITHUB_OUTPUT"
fi
```

Use the repository helper rather than GNU `timeout`: macOS does not provide the
latter, and the helper terminates the whole compiler process group before
returning the same exit-124 contract.

The job then finishes **normally** with a loud, explicit infra-skip, and the run can
reach a conclusive success/failure. Do **not** reach for job-level `continue-on-error`
here: GitHub documents it for a job that *fails*, and a timeout is reported as
*cancelled* — whether it covers that is a semantics gamble. Bounding the step is
correct by construction.

> A silent cancel is indistinguishable from "this workflow does nothing" — which is
> exactly how working coverage got written off as absent.
