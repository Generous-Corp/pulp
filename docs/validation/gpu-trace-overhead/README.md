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
  --trace test/fixtures/perfetto-gpu/healthy.pftrace \
  --trace-processor "$HOME/.pulp/tools/trace-processor/v57.2/mac-arm64/trace_processor_shell" \
  --question gpu-health \
  --source-revision "$(git rev-parse HEAD)" \
  --a2t-implementation-revision "$A2T_REVISION" \
  --equivalent-a2t-revision "$A2T_ORIGINAL_REVISION" \
  --output /tmp/gpu-trace-overhead.json
```

The script requires `pulp` and `pulp-mcp` to be regular sibling files. During
measurement `PATH` excludes their prefix and all checkout build directories;
MCP therefore succeeds only through its installed absolute-sibling binding.
Semantic parity is checked on every warm-up and measured trial.

The Horizon-A A2T implementation revision is inventoried before measurement,
and a stable patch ID binds pre-rebase and replayed commit identities.
It added offline CLI/MCP/SQL/docs/tests and no runtime/render/view/format or
Inspector producer path. Therefore new-producer runtime overhead is
`not-applicable-no-added-producer-cost`, not a timing pass. The canonical plan
must explicitly accept that disposition before A2T is formally complete.

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
`ui_correlation.open_command` plus bounded search terms. Human acceptance still
requires opening that exact SHA-256-identified artifact in Perfetto UI,
searching the emitted dominant stage and evidence ID, and confirming the cited
span is present on the expected track. Record the reviewer, date, artifact
digest, and observed span; an executable open command is not itself visual
inspection.

Perfetto is a localization tool, not an oracle for every platform state
machine. A real resize investigation demonstrated the correct evidence chain:
the trace showed paint was cheap and localized the delay to acquire/present and
compositor timing; a platform event-order harness then reproduced a redundant
same-size callback releasing retained content too early; a planted regression,
60 fps recording, and direct feel check validated the fix. Do not claim the
trace alone discovered the callback race, and do not turn that AppKit-specific
mechanism into a generic Pulp or Vellum contract.
