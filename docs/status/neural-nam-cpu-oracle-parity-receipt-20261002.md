# Neural NAM/TCN CPU-oracle parity receipt

**Status:** private adapter prototype landed; standalone numerical parity passed
for the pinned WaveNet A1 fixture; no Pulp ABI change.

## Finding

The model-neutral private contract is ready for a CPU adapter:

- `detail::StreamingModel` provides `spec()`, control-thread `prepare()`,
  callback-safe `process_cpu()`, `reset()`, `quiesce()`, and `release()`.
- `MicroTcnModel` is a contract fixture, not a NAM implementation. It proves
  causal state, partitioned blocks, reset epochs, and bounded storage, but its
  depthwise one-layer topology cannot establish NAM parity.
- The Pulp tree intentionally does not vendor GPU-NAM. The shipped
  documentation routes the plugin to `danielraffel/pulp-gpu-nam`, where the
  choc-only CPU oracle and `.nam` loaders are owned. A sibling checkout for
  investigation is pinned at `/Users/danielraffel/Code/pulp-gpu-nam` revision
  `014f24325a7f4211ee5052993badd9c90151b414`.

Pulp now has `detail::NamTcnStreamingAdapter`, a private callback-table bridge
that delegates prepared mono CPU blocks to a consumer-owned kernel without
copying or linking GPU-NAM/NAMCore code. Its tests prove stateful block
processing, reset, release, and non-mono rejection.

The active plan names GPU-NAM `origin/main` at `c5b3a1bc6` as the source review
pin and requires the MIT choc-only oracle. The older read-only audit recorded
source `00b333e1c03b180968d1c5d48376e5663c0ef19b`; these revisions differ from
the currently inspected checkout and must be reconciled before a parity result
is promoted.

## Smallest parity harness once the inputs are supplied

Add a private `NamTcnModel` implementing `StreamingModel`, backed by an owned
immutable parsed fixture and preallocated causal state. The harness should:

1. pin the GPU-NAM source revision, model-file SHA-256, architecture, and
   attribution receipt;
2. prepare one model at 48 kHz with 64- and 128-frame blocks;
3. render the same deterministic mono input through the GPU-NAM choc-only
   oracle and the adapter, after identical prewarm;
4. compare full output vectors with peak residual `<= 1e-6`;
5. rerun with alternate block partitions, call `reset()` and verify replay;
6. run a planted one-sample state offset and an unsupported-layer fixture, both
   of which must fail before audio preparation; and
7. run the existing callback allocation/lock probe against `process_cpu()`.

The adapter must remain private, use no external runtime source or headers,
and expose no new public ABI. The oracle and adapter call counts,
runtime/model hashes, sample rate, block sizes, state bytes, and residual must
be emitted in a machine-readable receipt.

## External parity experiment

The temporary harness was compiled outside Pulp from the adapter header and the
GPU-NAM oracle (no source was copied into Pulp):

```text
source_revision=014f24325a7f4211ee5052993badd9c90151b414
fixture=src/models/example.nam
fixture_sha256=66bda2b379289eff079c0755588bc9a92760654d9cc9af1b97cf30d0e92b167d
architecture=WaveNet-A1
sample_rate=48000
block_size=64
weights=131/131
max_residual=0
reset_replay_residual=0
```

The oracle was independently loaded and prewarmed with
`prewarm_block_aligned(64)`. The adapter delegated four 64-frame blocks through
the private callback bridge. Reset parity intentionally compares cold
`reset()` to cold `reset()`; prewarm is a separate preparation operation.

## Missing inputs / blocker

The following inputs remain before broader parity can be claimed:

- additional MIT-attributed `.nam` fixtures for the 128-frame and alternate
  architecture matrix, each with SHA-256 and expected architecture;
- the oracle's deterministic input/output vectors and prewarm/reset procedure;
- the exact choc commit/configuration used by the oracle; and
- permission to add the fixture to a non-shipped test corpus under its stated
  license.

The fixture above supplies the first input and the external experiment supplies
the first vector receipt. The committed Pulp test remains synthetic by design;
the external harness must stay downstream because the oracle source is not a
Pulp dependency.

## Fixture and architecture inventory

The pinned checkout contains only these model files (all MIT-attributed by
`src/models/ATTRIBUTION.md`):

| Fixture | Architecture | SHA-256 | Result |
|---|---|---|---|
| `example.nam` | WaveNet A1 | `66bda2b379289eff079c0755588bc9a92760654d9cc9af1b97cf30d0e92b167d` | adapter/oracle residual 0; 48 kHz; 64-frame blocks |
| `wavenet_a1_standard.nam` | WaveNet A1-Standard | `ceb53469a19ce278e2235da982ae676cb8d5451a8de22a7ecc7a2617d07224d1` | adapter/oracle residual 0; 64-frame blocks; file omits `sample_rate` |
| `lstm.nam` | LSTM | `df9f78c49f49c2bb32411df47e3f53746075adb206b92d017e06379d1e56234a` | adapter/oracle residual 0; 48 kHz; 64-frame blocks |

The standalone oracle checker reports `example.nam` weights `131/131` and
`wavenet_a1_standard.nam` weights `13802/13802`, with finite, nontrivial, causal
outputs for both. The LSTM adapter run also passed reset replay with residual 0.
The A1-Standard parity run used 48 kHz because the fixture does not declare a
sample rate; this is recorded as an evidence limitation.

No bundled A2, ConvNet, or Linear fixture exists. Their loaders and unit tests
are present in GPU-NAM source, but no real serialized artifact is available for
the private Pulp adapter to compare. A2/ConvNet/Linear parity therefore remains
open pending MIT-attributed fixtures and hashes.
