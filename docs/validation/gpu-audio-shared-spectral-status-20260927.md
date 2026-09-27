# Shared spectral SDK investigation record

Historical entries below retain their original source/build context. They are
not current installed-SDK acceptance claims.

Owner at initial implementation: sdk_buildtree_probe. Pulp branch feat/shared-spectral-sdk-20260927 at
/tmp/pulp-shared-spectral-sdk-20260927, based on c17fc3c. Spectr branch
/tmp/spectr-gpu-audio-validation-20260927, hardening commit be8fad6.

Spectr baseline hardening: governed installed SDK build and three focused CTests
passed; /tmp/spectr-stft-baseline-evidence-20260927/hardening-*.

Pulp implementation in progress: GpuSpectralMaskSession, immutable real mask,
Hann WOLA, persistent device history/OLA/normalization, imported slots and
prepared FFT graph. CPU host copies explicitly remain. No per-hop WebGPU
payload transfer intended. SharedIoProgramSession owns lifecycle; fixed hops
must be submitted contiguously. Intrinsic latency FFT+hop excludes caller lead.

Current probe uses existing c17fc3 archives plus NEW provider/session sources in
/tmp/pulp-shared-spectral-probe-build-20260927; this is provisional source proof,
not rebuilt installed SDK. CPU SpectralFrameEngine is independent oracle.
Initial configure flags incorrectly de-duplicated -isystem; local build recipe
corrected to -I. First build is still finishing its other object; no concurrent
build should be started in this directory.

Open: finish probe build; real Metal parity startup/tail/identity/nontrivial
mask/stereo/impulse; provider identity+transfer counters; failure lifecycle;
production CMake test registration; full target compile; installed SDK
consumer; Spectr adapter+continuous fallback and lead 1/2/4/8; contention and
reliability. No performance or completion claim yet.

## First real provider receipt

2026-09-27: provisional governed compile/link succeeded after correcting local
CMake flag quoting and WGSL reserved helper name. Runtime-v3 passed 48 stereo
hops FFT1024/hop256 with nontrivial low/high gain mask against actual CPU
SpectralFrameEngine: maximum absolute error 8.9407e-08, startup and zero-fed tail
included. Gap submissions refused. This is actual shared provider execution,
not the earlier staged GpuStft control; still not installed SDK or perf proof.

Public diagnostics/counters now added; build-v6/session84236 is verifying them.
Counters separate ordinary host slot copies from runtime WriteBuffer/CopyBuffer/
MapAsync calls, and expose authenticated Dawn revision/adapter, imports,
retirements and confirmed physical release. Device initialization uploads remain
outside runtime counters. More parity sizes, explicit impulse latency, slot
exhaustion/failure lifecycle and downstream installed consumer remain open.

Runtime-v4 passed with public diagnostics: Apple M5 Max, authenticated Dawn
1e897275172a23f27b0022fa6beae3084ed54a9b, 6 imports, 48 successful retirements,
zero runtime WebGPU payload calls, 98304 host input bytes; physical release
confirmed. build-v6 and runtime-v4 are current evidence before further tests.

## Review and expanded acceptance

Root review fixes: raw provider observer is assigned only from retained session
ownership after prepare, including failed preparations. Diagnostics distinguish
header/native/proc agreement from configured manifest revision verification.
Runtime copy counters count all runtime WebGPU buffer calls, not just payload.
Result success is physical success, not deadline acceptance. Process-unique
stream epochs prevent old results aliasing a recreated session.

build-v8/runtime-v6 passed three real-Metal cases: FFT256 identity impulse
verifies actual FFT+hop latency (error 1.78814e-07), FFT1024 stereo static mask
8.9407e-08, FFT8192 stereo static mask 5.96046e-08. 48 hops each includes startup
and drained tail, independent CPU SpectralFrameEngine. Extra cases cover
invalid shape/NaN gains, nonfinite oracle negative controls, gap refusal, late
physical completion, slot exhaustion followed by same-sequence retry, distinct
session epochs and release with in-flight work. Runtime WebGPU calls all zero.
Configured revision verified remains false in the provisional build, explicitly
pending the production testsOFF exact-provider fix owned by planning_audit.
GPU-off source compilation with -Wall -Wextra -Werror also passes.

## Lifecycle review and publication preparation

Review found two wrapper issues: a failed epoch could expose a later physical
success, and the pre-release diagnostics snapshot omitted retirements completed
during release. Commit b8d7ffd516 fixes both. A private result consumer now
discards later results from poisoned history. A generic release observer takes
final counters only after the arena physical barrier succeeds and before the
provider is destroyed; the existing release symbol remains available.

Focused deterministic validation passed 190 assertions in 15 cases. An isolated
copy with only the poisoned-state guard removed failed the three expected
assertions: later delivery, copied byte count, and overwritten output. The
Dawn-enabled wrapper also compiled. Receipt:
`/tmp/pulp-spectral-lifecycle-probe/receipt.json`. The hardware probe now checks
all five final retirements after releasing one hop in flight; that strengthened
hardware assertion remains pending on the authenticated combined SDK.

Publication uses a fresh main-based branch, with source commits 8fc7c916c8,
1ad61b59b0, 0e4b0b9f17, and b8d7ffd516 applied without the unrelated capability
PR ancestry. SDK-only configured-provider authentication requires the separate
production-identity change f9ccca1b11. Capability export and downstream SDK
pinning are separate integration changes. No realtime or performance verdict
is assigned by these correctness tests.
