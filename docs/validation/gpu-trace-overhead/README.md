# GPU trace-analysis overhead acceptance

`tools/scripts/gpu_trace_overhead_acceptance.py` measures the closed offline
GPU questions through an installed `pulp`/`pulp-mcp` sibling pair. It runs five
warm-ups, 30 alternating paired trials, and 20 alternating fresh-process
trials by default. The receipt retains every raw duration, executable and trace
digest, host/GPU identity, median, p95, median absolute deviation, and a paired
bootstrap confidence interval.

This harness answers a deliberately narrow question: how much wall time does
the installed offline analysis path take, and do CLI and MCP return identical
typed evidence from the same SDK-matched trace processor and saved artifact?
It does not claim that the saved fixture used this machine's GPU.

```bash
python3 tools/scripts/gpu_trace_overhead_acceptance.py \
  --cli /installed/prefix/bin/pulp \
  --mcp /installed/prefix/bin/pulp-mcp \
  --trace test/fixtures/perfetto-gpu/first-frame-pipeline-upload-stall.pftrace \
  --trace-processor "$HOME/.pulp/tools/trace-processor/v57.2/mac-arm64/trace_processor_shell" \
  --question gpu-startup \
  --source-revision "$(git rev-parse HEAD)" \
  --mcp-source-revision "$(git rev-parse HEAD)" \
  --plan-revision "$PLAN_REVISION" \
  --plan-sha256 "$PLAN_SHA256" \
  --planning-repository "$PWD/planning" \
  --prior-human-review-receipt \
    docs/validation/gpu-trace-overhead/m3-a2t-offline-analysis-20260828.json \
  --output /tmp/gpu-trace-overhead.json
```

The recorder requires an exact clean canonical Pulp worktree at
`--source-revision`, plus a clean Release installed prefix whose generated
`include/pulp/runtime/build_info.hpp` identifies that revision. `pulp` and
`pulp-mcp` must be executable regular sibling files. During
measurement `PATH` excludes their prefix and all checkout build directories;
MCP therefore succeeds only through its installed absolute-sibling binding.
Semantic parity is checked on every warm-up and measured trial. Both binaries
must be built from the same source revision; a distinct
`--mcp-source-revision` is required and accepted only when it exactly matches
the lowercase 40-hex `--source-revision`. The installed build stamp is parsed
and checked rather than trusting those declarations alone. The accepted plan
is likewise read from its immutable Git object and checked against its Git blob
and SHA-256 identities.

The no-added-producer disposition is not caller-selected and does not label
whole mixed-purpose commits as A2T work. The recorder loads the checked-in
`gpu_trace_overhead_scope.json` contract, verifies its canonical-plan 29-path
implementation identity, and recomputes an immutable-pre-A2T-base to exact
source-head tree delta over the complete current A2T path contract. It records
exact base/source blobs plus path-limited touching history. A commit that also
changes unrelated A3/A4/tooling code contributes only its A2T path delta; its
other paths neither contaminate nor justify this disposition. Recorder and
verifier changes at the final source head are included through their exact
source blobs without requiring a self-referential commit list.

Manifest completeness is independently derived from immutable Git objects:
the accepted patch's exact 29 paths, every tracked Perfetto-GPU fixture, the
bounded Rust trace-command and A2T acceptance-script families, and exact stable
A2T schema/view/tool identifiers within fixed behavior roots. The discovery
roots include the plan's product-producer prefixes, where the required
`debug.gpu_evidence_id` key would make a new producer path fail the no-producer
contract. Generic symptom prose such as “slow GPU startup” is not an identifier
and cannot expand the scope by substring accident.

New v2 receipts also bind `integration_head` plus the exact Git blobs for the
Rust analyzer, all three closed PerfettoSQL views, CLI/MCP dispatch surfaces,
processor pin sources, recorder/verifier, and every checked-in trace in the
required replay matrix. Validation resolves those blobs at
the historical integration head, current `HEAD`, and the current checkout, so
an analyzer, SQL, MCP projection, or fixture edit makes the receipt stale even
when its saved timing JSON is unchanged. The committed
`m3-a2t-offline-analysis-20260828.json` predates this field and is intentionally
stale after the current analyzer hardening; regenerate it only with the final
installed CLI/MCP and pinned trace-processor replay.

The Rust unit contract and real fixture integration wrapper are always
registered when the experimental Rust CLI is enabled. The wrapper resolves an
explicit `PULP_TRACE_PROCESSOR` first, then Pulp's canonical pinned cache, and
returns visible CTest skip code 77 when neither exists. A configured test is
therefore never silently absent. The acceptance recorder additionally rejects
a processor outside the canonical cache, with the wrong platform/version/hash,
or whose `--version` output does not match Pulp's v57.2 SDK dependency.

Before timing, the recorder replays the healthy, shader-compile failure,
blank-readback failure, device-loss, acquire/present wall-time-only,
first-frame pipeline/upload stall, incomplete-capture, and wrong-category
fixtures twice through the installed CLI and once through the installed MCP.
It requires deterministic checked-view replay, typed exit/`isError` behavior,
text/structured MCP identity, all semantic fields, and the intended
verdict/stage/action. The terminal verifier correlates the timed result to that
same replay row and checked-in artifact digest.

The recorder derives the complete path-scoped A2T tree delta from the immutable
pre-A2T boundary to the exact measured source head. A caller cannot shorten or
select its history. The manifest's stable patch ID binds the canonical plan's
original/replayed implementation to its integrated equivalent; later changes
are bound by their exact final blobs and path-limited touching history. This
scope contains offline CLI/MCP/SQL/docs/tests and no runtime/render/view/format
or Inspector producer path. Therefore new-producer runtime overhead is
`not-applicable-no-added-producer-cost`, not a timing pass. The receipt binds
the exact canonical plan revision and digest that accepts this Horizon-A
disposition while preserving the full producer/xrun protocol for B6.

When B6 adds Vellum-owned render producer instrumentation, compare the same
optimized product workload and adapter against the pre-change baseline in
three configurations:

1. tracing compiled out;
2. tracing compiled in with no active session; and
3. an active bounded 128 MiB capture.

Use five warm-ups and 30 measured trials for steady-state work plus 20
fresh-process trials for startup. Preserve raw samples and the same identity
fields as this receipt. Grade the median/p95 deltas against the canonical plan's
1%/2%, 2%/5%, and 5%/10% ceilings respectively, and prove the active capture
adds no xrun. Offline analysis cannot waive or stand in for that B6 gate.

## Human Perfetto correlation

Automation proves that the typed result cites the saved artifact and emits an
`ui_correlation.open_command` plus bounded search terms. The committed M3
receipt also records the completed visual acceptance against the exact
`9fd7cf0d...` artifact: Perfetto UI selected `gpu_pipeline_prepare` (1.8 ms)
and `gpu_resource_upload` (0.9 ms) on the expected GPU track and displayed the
shared `4444...4444` evidence ID plus the expected frame and sequence fields.
The fixture was delivered through Perfetto's official localhost embedding
protocol with `localOnly` browser-memory handling; it was not uploaded or
shared. Future receipts must retain the reviewer, date, artifact digest, UI
revision, and observed span details; an executable open command is not itself
visual inspection.

Regenerating analyzer timings does not repeat or silently manufacture that
visual acceptance. For a `gpu-startup` rerun, pass the prior accepted A2T
receipt with `--prior-human-review-receipt`. The generator carries its
`human_perfetto_ui_correlation` root object forward verbatim and records
`acceptance.human_perfetto_ui_correlation: pass` only when the prior receipt is
also a `gpu-startup` receipt with passing human acceptance and its trace
artifact SHA-256 exactly matches the trace being measured. A missing object,
different question, non-passing acceptance, or changed artifact fails closed.
The current top two typed contributors must also match the human-observed span
name, duration, evidence ID, frame index, sequence, and health state exactly.
Do not pass this option for `gpu-health` or `gpu-probe`; those runs cannot
inherit a startup UI review.

After recording, verify terminal status from the exact integration checkout:

```bash
python3 tools/scripts/verify_gpu_trace_overhead_acceptance.py \
  /tmp/gpu-trace-overhead.json --repository "$PWD"
```

`--allow-nonterminal` exists only for inspecting an intentionally incomplete
draft. It must not be used by a checked-in final acceptance gate. The existing
`m3-a2t-offline-analysis-20260828.json` is historical: it predates v2 installed
provenance, complete fixture replay, expanded semantic parity, and exact
same-artifact contributor correlation, so it is not terminal evidence.

Perfetto is a localization tool, not an oracle for every platform state
machine. A real resize investigation demonstrated the correct evidence chain:
the trace showed paint was cheap and localized the delay to acquire/present and
compositor timing; a platform event-order harness then reproduced a redundant
same-size callback releasing retained content too early; a planted regression,
60 fps recording, and direct feel check validated the fix. Do not claim the
trace alone discovered the callback race, and do not turn that AppKit-specific
mechanism into a generic Pulp or Vellum contract.
