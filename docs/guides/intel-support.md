# macOS Intel (x86_64) support

Pulp builds and ships macOS **arm64** (Apple Silicon) as its primary, `usable`
target. macOS **x86_64** (Intel) and **universal** (`arm64;x86_64`) builds are
supported — they configure and build via `-DCMAKE_OSX_ARCHITECTURES` — but ship
`experimental`. This guide documents how Intel portability is verified, why the
verification is tiered "regular, not every PR", and exactly what the cheap
Tier-0 canary can and cannot catch.

The source is arch-portable by construction: SIMD goes through Highway's runtime
dispatch (no raw NEON in `core/`), and the toolchain selects Skia / wgpu / V8
slices off the **target** arch (`CMAKE_OSX_ARCHITECTURES`), not the host. So
"arm64-only" was a build-wiring limitation, not a port — and the risk that
remains is narrow: arch-gated `#if` blocks, build-system arch selection, and the
handful of things that only surface at runtime on real Intel silicon or a real
Intel/AMD GPU.

## The tiering

| Tier | What | Where | When | Blocking? |
|------|------|-------|------|-----------|
| 0 | Intel **canary**: static lint + `-DPULP_ENABLE_GPU=OFF -DCMAKE_OSX_ARCHITECTURES=x86_64` compile of `pulp-runtime pulp-signal pulp-platform pulp-state pulp-format` | inside the existing ARM macOS job (`build.yml`), gated on the `PULP_INTEL_CANARY` repo variable | every PR (opt-in; forks pay nothing) | yes, on `danielraffel/pulp` |
| 1 | Path-triggered **advisory** x86_64 build + full ctest under Rosetta | `intel-portability.yml`, **stable** `macos-15` (arm64+Rosetta) | PRs that touch arch-sensitive paths | no (advisory) |
| 2 | **Nightly** native Intel (job A) + universal cross-check (job B) | `nightly-intel.yml`: job A on `macos-15-intel`, job B on `macos-15` | cron, off-peak | no (opens a watchdog issue) |
| 3 | **Release gate**: universal build + `lipo -archs` + `codesign --verify` + dual-arch `auval` | `release-cli.yml` `universal-arch-gate` | on tag / release dispatch | advisory as of 2026-07-11 (see note) |

The Tier-3 gate was blocking, but as of 2026-07-11 it is **advisory**
(`continue-on-error`, dropped from the `release` job's required `if:`): a known
`auval` "Bad Max Frames" flake was wedging every publish. Re-blocking it — fix
the AU max-frames guard in `core/format/src/au_v2_adapter.cpp`, then restore the
hard `needs`/`if` requirement — is a tracked follow-up.

### Shipped Intel artifacts — currently DISABLED (2026-07-11)

Tier 3 *validates* a universal build; it never published an installable Intel
binary. The separate **installable** `darwin-x64` release leg (a native thin
matrix row on `macos-15-intel` that shipped `pulp-darwin-x64.tar.gz` +
`pulp-sdk-darwin-x64.tar.gz`) is **temporarily removed** from `release-cli.yml`.
The GitHub-hosted `macos-15-intel` image CPU-pegs on a full CLI+SDK build and
reliably blows any sane timeout, shipping no artifact — and its timeout
*cancellation* (not a clean failure) turned `build-cli`'s matrix aggregate
`cancelled` and skipped the whole `release` job. Until the reliable path lands,
Intel-Mac users **source-build**.

The reliable x86_64-macOS path is the Tart Intel cross-build golden VM (arm64
host cross-compiling x86_64); Intel returns to the release through that lane.
Wiring it in still needs the Apple `CMAKE_OSX_ARCHITECTURES` branch for the
`if(WIN32)`-gated wgpu arch-forcing in `PulpDependencies.cmake` plus arch-isolated
Skia extraction (`planning/2026-07-10-intel-mac-cli-support.md`, Option A).
`macos-15-intel` remains the pinned stable hosted x86_64 image for the Tier-2
nightly and the successor plan when it EOLs (~Aug 2027).

Runner discipline is absolute: **no Intel work ever routes to the self-hosted
Mac Studios** that host the required `macos` gate, and **Namespace is never
used** (cost). `danielraffel/pulp` is a public repo, so GitHub-hosted macOS
minutes — including the native `macos-15-intel` runner — are free; the only
budget that matters is wall-clock, runner flakiness, and Studio capacity.

### Why `macos-15-intel` is quarantined to the nightly

`macos-15-intel` is a **real** GitHub-hosted runner with a **native Intel CPU +
toolchain** (no Rosetta) — it removes Rosetta's SSE4.2/AVX2 cap and exercises the
real Intel linker. But it has documented flake: C++ linker-image errors and
100%-CPU pegging. So:

- **Tier 1** (per-PR) runs on the **stable** `macos-15` with a thin-x86_64 build
  whose test binaries run transparently under Rosetta — a red there is almost
  always a real regression, and it never blocks (advisory).
- **Tier 2 job A** is the only lane that touches `macos-15-intel`, at night,
  where a flaky red costs nobody a merge. Its watchdog **classifies** failures:
  an *infra* signature (linker-image error, `Killed: 9`, timeout/CPU-peg) is
  labelled `runner-infra` and does **not** page; a product failure does. Job B
  (universal on the stable runner) keeps the x86_64 signal alive whenever job A's
  runner is down.

## The honest canary catch/miss list

The Tier-0 canary (`tools/scripts/intel_canary_lint.py` + the GPU-off x86_64
compile) is a **cheap early-warning, never a substitute for Tier 2**. Be honest
about its reach.

**It CATCHES:**

- `#if defined(__aarch64__)` / `__ARM_NEON` gating that drops the x86 fallback
  — surfaces as a compile error in the x86_64 GPU-off build.
- Raw NEON intrinsics / `arm_neon.h` used outside an ARM guard in `core/`
  (lint class 1).
- ARM-gated SIMD `#if` chains with no `__x86_64__`/`__SSE*` sibling branch and
  no `#else` fallback (lint class 2).
- Hardcoded `darwin-arm64` / `mac-arm64` / `aarch64` asset/arch strings in
  `tools/cmake/**` + `tools/scripts/fetch_*` (lint class 3).
- `CMAKE_SYSTEM_PROCESSOR` (the **host** arch) driving an Apple **target**-arch
  decision in a file that never consults `CMAKE_OSX_ARCHITECTURES` (lint
  class 4).
- Hardcoded `CMAKE_OSX_ARCHITECTURES=arm64` (arm64-only) in `tools/cmake/**`
  (lint class 5).
- Arch-dependent `static_assert` / size assumptions and `long double` layout
  drift, and missing SSE headers — via the compile half.

**It MISSES (this is what Tier 2/3 exist for):**

- **Everything GPU-gated out.** `-DPULP_ENABLE_GPU=OFF` excludes `core/render`
  (CMakeLists ~256–258), `inspect` (~303–304), and `tools/cli` (~351–358). Skia
  / Dawn / wgpu **link and runtime** arch bugs — the ones most likely to bite —
  are invisible to the canary. Only the Tier-2 native build and Tier-3 universal
  gate exercise them.
- **Runtime SIMD dispatch.** Highway picks its target at runtime; a wrong
  dispatch or an AVX-path bug compiles fine and only shows up when the x86_64
  binary actually runs (Tier 1 ctest under Rosetta, Tier 2 native).
- **Metal on a real Intel/AMD GPU.** GitHub's runners are VMs without a
  representative GPU, so no CI tier reaches it (see "Residual risk" below).
- **CMake fetch-path arch selection at runtime.** The lint half covers only the
  hardcoded-string subset; a logic bug that fetches the wrong slice surfaces at
  Tier-2/3 build time.

## Why `PULP_INTEL_CANARY` defaults OFF

The option is `OFF` by default so **external cloners pay nothing**. Most people
who vendor Pulp ship a single-arch Apple-Silicon plugin and never build for
Intel; forcing a cross-arch lint + compile on them would be a pure tax with no
benefit. `danielraffel/pulp` — which does care about the Intel story — opts in
by setting the `PULP_INTEL_CANARY` repo variable to `1`, exactly like
`PULP_REQUIRE_GPU_FOR_SDK` is flipped on only in the release workflow. Forks
inherit `OFF` and their CI step is skipped.

## "Regular, not every PR" — the owner's rationale

A full macOS x86_64 build on **every** PR would either (a) burn a self-hosted
Studio slot that must stay free for the required `macos` gate, or (b) add
20-plus minutes of GitHub-hosted wall-clock to every PR for a portability axis
that changes rarely. Neither is worth it. Instead:

- The **cheapest** signal (Tier 0) rides for free inside the ARM job on every
  PR and converts the single most common regression — compile-time arch gating
  — from a nightly-latency signal into a ~4-minute PR-time signal.
- The **full** x86_64 build+test (Tier 1) fires only when a PR touches
  arch-sensitive paths, and even then it is advisory.
- The **expensive, complete** verification (Tier 2 native Intel + Tier 3
  universal gate) runs on a schedule and at release — "tested when it really
  really matters".

## Promotion criteria (so it isn't ambiguous)

- **Tier 1 → required-on-path-match** after **14 consecutive green Tier-2
  nightlies** with **zero `runner-infra`-only reds in the last 7**. (An
  infra-only red does not reset the green streak, but a run of them delays
  promotion — that is why the watchdog tracks infra-only reds even though it
  does not page on them.)
- **`macos-15-intel` may join Tier 1** (i.e. become a per-PR runner) **only
  after 30 days with no infra-signature failure.** Until then it stays
  quarantined in the nightly.

## Tier 4 is CUT

A dedicated Intel Mac / a tartci Intel host (the former "Phase 2") is **cut**.
`macos-15-intel` already covers the **native Intel CPU + toolchain**, which was
the whole reason a physical Intel Mac was ever contemplated. What remains
genuinely untestable in CI is narrow:

- **Metal on real AMD/Intel GPUs.** GitHub's macOS runners are VMs without a
  representative discrete GPU, so no tier — not even native `macos-15-intel` —
  exercises a real Intel/AMD Metal driver.
- **Intel-native DAW-in-the-loop.** Hosting the plugin inside Logic / Live /
  Cubase on real Intel hardware.

Those two gaps are exactly the `experimental` justification recorded in
`docs/status/support-matrix.yaml`. Revisit a dedicated Intel host only on a
user-reported Intel-GPU or Intel-DAW bug — not preemptively.

## Running the canary locally

```bash
# The static lint (sub-second). Whole tree:
python3 tools/scripts/intel_canary_lint.py --mode=tree
# Only files changed vs a base ref (the PR-fast path):
python3 tools/scripts/intel_canary_lint.py --mode=changed --base origin/main
# The lint's own self-test:
python3 tools/scripts/test_intel_canary_lint.py

# The configure-time gate (matches CI's Tier-0 opt-in):
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_INTEL_CANARY=ON
```

If the lint flags a line that is genuinely arch-aware (correct arch-selection
logic, a host-arch probe, or a deliberately arm64-only Apple toolchain), record
it — **with a reason** — in `tools/scripts/intel_canary_allowlist.txt`. Do
**not** weaken the lint: a finding on a healthy tree is a real regression to fix
at the source. See the `.agents/skills/intel-canary` skill for the maintenance
contract.
