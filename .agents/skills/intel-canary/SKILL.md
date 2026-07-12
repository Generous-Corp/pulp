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

## What this skill covers

- `tools/scripts/intel_canary_lint.py` — the Tier-0 static lint (5 classes).
- `tools/scripts/intel_canary_allowlist.txt` — its exemption list.
- `tools/scripts/test_intel_canary_lint.py` — the lint's self-test.
- The `PULP_INTEL_CANARY` option in the root `CMakeLists.txt`.
- The Tier 0-3 workflows: `build.yml` (canary step), `intel-portability.yml`
  (Tier 1 advisory PR lane), `nightly-intel.yml` (Tier 2), and the
  `universal-crosscheck` job in `nightly-intel.yml` (Tier 3, nightly).

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
  quarantined to Tier-2 job A with `timeout-minutes: 120` and an infra-vs-product
  watchdog classifier. Never route it to a per-PR required lane, and never route
  ANY Intel work to the self-hosted Studios or to Namespace.
- **A universal wgpu dylib must be re-signed after `lipo`** (G3 lesson): a raw
  fat dylib fails `codesign --verify` and the arm64 slice is killed at load. The
  Tier-2/3 `check_bundle_architectures.py --strict` assertions verify BOTH
  `lipo -archs` and `codesign --verify` on every embedded dylib for this reason.

## Editing checklist

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
