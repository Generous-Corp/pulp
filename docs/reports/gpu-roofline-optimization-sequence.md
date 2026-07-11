# GPU roofline & optimization sequence — `gpu_audio` compute passes

**Status:** Reference. Describes the standing instrumentation and the ordered
optimization program for Pulp's GPU audio/DSP kernels.
**Instruments:** the roofline/occupancy harness (`pulp-gpu-roofline-harness`,
landed in #5877) and the compute-pass GPU-busy timestamp probes
(`GpuCompute::fft_forward_timed`, `GpuCompute::multi_convolve_timed` — the
latter landed in #5944).
**Source analysis:** the audio-pipeline roofline audit (findings #1–#8) plus the
WaveNet occupancy audit. This document is the code-side companion: how to
*measure*, what the measurements *said*, and the *order* to act on them.

---

## 0. The governing principle

> A GPU *defeat* measured through a kernel running one to three orders of
> magnitude under the machine is not a defeat. It is a bug report with a
> conclusion attached.

Several of Pulp's recorded GPU "losses" were measured against kernels whose
*structure* — not the hardware — cost 10×–1000×. Before any GPU-vs-CPU verdict
is trusted (or an async-execution decision is sized against a per-block GPU
cost), the kernel behind that verdict must be shown to be roofline-healthy. The
harness and the timestamp probes exist to make that check cheap and repeatable.

Two failure modes this instrumentation is built to catch:

- **False losses** — a kernel blamed on "dispatch + readback overhead" that is
  actually bandwidth- or serialized-ALU-bound doing work it should not do.
- **False wins** — a "GPU win" that was a Dawn error-buffer no-op or a
  poll-latency artifact. (The corpus has caught two of these; the roofline pass
  is the symmetric guard for losses.)

---

## 1. The instruments

### 1.1 Roofline / occupancy harness (`pulp-gpu-roofline-harness`, #5877)

Tooling target (built only under `PULP_ENABLE_GPU`; needs a real GPU/Dawn
device). It drives each MAC-dense `pulp::render::GpuCompute` pass over escalating
workloads and prints one ranked table. Per pass it reports:

| Column | Meaning | What a bad value tells you |
|---|---|---|
| **GMAC/s achieved** | measured multiply-adds ÷ measured GPU block time | low vs peak ⇒ the kernel is leaving the machine idle |
| **device roofline GMAC/s** | empirical fused-MAC peak from a large, well-parallel matmul | the ceiling every other pass is held against |
| **roofline gap** | `peak ÷ achieved` | the headroom multiplier — how far under the part you run |
| **lane occupancy** | lanes doing useful work ÷ lanes launched | low ⇒ one-thread-per-output geometry starving the SIMD width |
| **workgroups dispatched** | SM/core-coverage proxy | a single workgroup pins the whole grid to one core |
| **serial MAC depth** | the reduction each lane walks alone | large ⇒ a serialized-ALU critical path masquerading as "API overhead" |

Rows are ranked by `roofline gap × hotness` (hotness = measured block time), so
the biggest, hottest gaps float to the top. This is the instrument that first
confirmed the WaveNet one-thread-per-sample occupancy gap and surfaced its
siblings (matmul / additive / modal are all one-thread-per-output).

Run it:

```sh
cmake --build <build> --target pulp-gpu-roofline-harness   # needs PULP_ENABLE_GPU
./<build>/.../pulp-gpu-roofline-harness
```

### 1.2 Compute-pass GPU-busy timestamps

The harness uses wall time; wall time conflates GPU work with upload and the
blocking readback. To separate them, two passes expose a timestamp-query variant
that brackets the *compute passes only* and returns GPU-busy microseconds
(`-1` when timestamps are unavailable):

- `fft_forward_timed` — the FFT path.
- `multi_convolve_timed` (#5944) — the fused multi-IR sequence (forward FFT →
  broadcast multiply → batched inverse FFT → pan-combine).

The diagnostic is the **busy/wall ratio**. Rising toward ~1.0 as the workload
grows means the cost is *real GPU work* (compute- or bandwidth-bound) and a fix
must cut the work. Staying low means the cost is upload/readback/dispatch and a
kernel rewrite will not help. This is the single measurement that distinguishes
"the kernel is slow" from "the round trip is slow."

---

## 2. What the measurements said

### 2.1 `multi_convolve` is GPU-work-bound, not overhead-bound (measured, #5944)

Measured on Apple Silicon / Metal, `block=512`, `num_ir=32`, via
`multi_convolve_timed`:

| IR (s) | n (FFT) | wall (µs) | GPU-busy (µs) | busy/wall |
|---:|---:|---:|---:|---:|
| 0.10 | 8192 | 784.3 | 393.2 | 50% |
| 0.50 | 32768 | 852.0 | 524.3 | 62% |
| 2.00 | 131072 | 2483.9 | 2097.2 | 84% |

`busy/wall` climbing toward 1.0 as `n` grows **confirms the cost is genuine GPU
work whose size tracks the full FFT length `n`** (set by IR length), not
dispatch/readback overhead. The current kernel runs a *non-partitioned*
convolution: every 512-sample block pays a full `n`-point forward FFT, a
broadcast multiply, and `num_ir` batched `n`-point inverse FFTs. At the audited
point the inverse batch alone moves ≈983 MB/block — the kernel is at the
device's memory-bandwidth roofline doing work it should not be doing at all.

**Consequence for the record:** the SuperConvolver v1.2 GPU-KILL verdict was
measured against this kernel while the CPU comparator had already been upgraded
to a partitioned real-FFT FDL. That comparison is asymmetric. The measured
baseline is now in; the go/no-go on building a GPU-resident partitioned FDL
(≈500 lines; predicted **10–20×**, *not yet built or measured*) is an owner
decision, and landing it re-opens the KILL verdict.

### 2.2 "Small-buffer collapse" is a serialized-ALU constant (WaveNet / conv_stack)

The WaveNet and `conv_stack_forward` kernels use one thread per audio sample,
each thread walking `~L × 2C²K` MACs serially, with layers strictly sequential.
The harness surfaces this directly: low occupancy (only `B` of the launched
lanes active), a single or few workgroups at small block sizes, and a large
per-thread serial MAC depth. The reported "realtime ratio collapses 9.4×→2.7×
from 256→64 buffers" is largely this serialized-ALU critical path
(`L × 2C²K × ALU latency`) — a block-size-independent constant — **not**
amortized API overhead.

---

## 3. The optimization sequence

Ordered by leverage-per-effort. Each item names the harness/probe signal that
surfaces it and its status. **Measured** speedups cite the landing PR; anything
marked **predicted** is an unbuilt estimate and must be re-measured before it is
trusted or used to size a downstream decision.

| # | Kernel / finding | Surfacing signal | Speedup | Status |
|---|---|---|---|---|
| 4 | `LadderFilter` runs 8× `std::tanh`/sample; `FastMath::tanh` shipped unused | CPU hot path (not GPU) | **1.43× measured** (Padé division + serial stage chain, not the predicted 4×) | **Shipped #5880** |
| 5 | `GpuConvolver` did serial per-channel round trips; `convolve_batch` unused | wall time dominated by per-channel map round trips | **2.02× stereo / 7.45× 8-ch measured** | **Shipped #5883** |
| 3 | Spectral smear was O(n·radius·layers) brute force (CPU **and** GPU) | CPU cost; GPU blur term in harness | **16.8× measured on the smear term** (O(n·r·L)→O(n) running sum; float drift 7e-7) | **Shipped #5927** (CPU); GPU shared-memory tile is a follow-up |
| 2 | `conv_stack_forward` = WaveNet one-thread-per-sample geometry (2nd instance) | low occupancy + high serial MAC depth | **10–100× parallelism predicted**; block-parallel WaveNet kernel landed | **Shipped #5878** (WaveNet); occupancy follow-ups #5967. `conv_stack` reparallelization remains |
| 1 | Multi-IR conv: full-length FFT ping-pong at the bandwidth roofline | `multi_convolve_timed` busy/wall → 1.0 | **10–20× predicted** (GPU partitioned FDL, ≈500 lines) | **Measured & confirmed** (#5944); FDL **not built** — owner go/no-go, re-opens KILL verdict |
| 6 | Stockham FFT issues one global-memory round trip per radix-2 stage + recomputes twiddles | taxes every FFT-based verdict | **5–10× predicted** on FFT paths (workgroup-shared-memory stage fusion) | Open (2–3 d) |
| 7 | CPU FFT taxes: complex-FFT-of-real, scalar radix-2 off-Apple, decorative Highway | CPU-side | **~2× predicted** for the real-FFT refactor (1–2 d, touches the IR-swap struct); deinterleave/Highway downgraded to cleanups | Re-scoped; deferred pending decision |
| 8 | Latent: granular re-upload, untiled matmul, per-call buffer creation | not on a product path today | n/a | Document or gate |

### Reading the sequence

1. **Cheap, unconditional wins (#4, #5):** already shipped. They competed with
   nothing and touch realtime-thread voices (#4) and the stereo/multi-channel
   convolver (#5).
2. **Strategy-correcting re-baselines (#1, #2, #3, #6):** these do not merely
   make a kernel faster — they correct *architectural* numbers. The corpus made
   decisions (the SuperConvolver GPU kill, the "GPU audio is dispatch/readback
   bound" doctrine, the fixed-512 NAM re-block latency, the spectral crossover
   tables, the public `gpu-audio-sdk.md` guidance) on measurements taken through
   under-running kernels. Re-measuring #1/#2 with the timestamp path *before*
   committing further async-execution architecture is hours of work and either
   confirms or refutes the whole re-baseline. In particular, an async layer sized
   to *hide* a per-block GPU cost that #1/#2 would shrink 10× may be
   compensating for a kernel artifact.
3. **What this does not overturn:** the ~0.5 ms readback map-latency floor is
   real physics; single-stereo convolution genuinely belongs on the CPU; the
   transport's fixed-latency-proxy model is the right shape; and the safe-
   reclamation primitives (`Slot`/`Handoff`) are untouched by any of this.

---

## 4. Re-measurement runbook

When re-baselining a GPU verdict or reviewing a new GPU DSP pass:

1. **Build the harness** and run it on the target device:
   `cmake --build <build> --target pulp-gpu-roofline-harness` (with
   `PULP_ENABLE_GPU`), then run the binary. Note backend, vendor, and whether
   `timestamp_query` is available (busy/wall needs it).
2. **Read the ranked table top-down.** A large roofline gap with low occupancy
   or a single workgroup is a parallelism defect (one-thread-per-output). A
   large gap at *full* occupancy is a bandwidth or algorithm defect (wrong
   FFT size, untiled matmul).
3. **Separate work from overhead** with the `*_timed` probe for the pass. If
   busy/wall is low, a kernel rewrite will not move the number — look at upload
   and readback. If it rises toward 1.0 with workload, the work is real and the
   algorithm must change.
4. **Predicted vs measured.** Every speedup in §3 marked *predicted* is an
   estimate. Before it informs a product or architecture decision, build the
   change behind a flag and re-run steps 1–3 — the corpus has twice found that a
   predicted multiple did not survive contact with the hardware (#4: 4×→1.43×).

---

## 5. Policy: new GPU DSP ships with a CPU twin and a bit-exactness test

The bit-matched CPU↔GPU pattern (e.g. `spectral_stack`, WaveNet determinism
tests) is the SDK's differentiator. Promote it from folklore to policy: **new
GPU DSP ships with a CPU twin and a bit-exactness test, or documents why not.**
Where a path is deliberately nondeterministic for realtime (the GPU convolver's
miss-substitution timing), say so at the API and point to the offline render as
the deterministic path.

---

## Corrections filed against the corpus

These prior claims were measured through under-running kernels and should be
read with this document:

- SuperConvolver v1.2 KILL verdict — measured asymmetrically (partitioned CPU vs
  non-partitioned GPU); contingent on a GPU FDL.
- "The dominant cost is dispatch + sync + copy overhead, not FLOPs" — for the
  multi-IR path this is substantially a kernel artifact (§2.1).
- NAM 9.4×→2.7× buffer collapse — a serialized-ALU constant, not amortized API
  overhead (§2.2).
- `gpu-audio-sdk.md` public guidance — measured through an 11-pass Stockham FFT
  (#6); revisit after stage fusion.
