# Graph-agent status guard

`tools/graph/graph_stall_guard.py` is the narrow liveness check for graph
implementation lanes. Existing queue and merge watchdogs observe GitHub jobs;
this guard observes the agent's own progress evidence.

The coordinator writes a JSON snapshot with schema
`pulp.graph_agent_status.v1`. It contains `generated_at` and a non-empty
`lanes` array. Each lane has:

- `lane`: stable graph roadmap ID such as `G-7` or `A9`.
- `status`: `ACTIVE`, `PARTIAL`, `BLOCKED`, or `DONE`.
- `heartbeat_at`: required for `ACTIVE` and `PARTIAL`; the default freshness
  limit is 90 minutes.
- `receipt_path` and `receipt_sha256`: required for `ACTIVE`, `PARTIAL`, and
  `DONE`. The path is relative to the manifest directory and the digest must
  match the file bytes.
- `pr_required` and `pr`: when a lane owns a PR, set `pr_required: true` and
  provide the canonical pull URL, positive number, full 40-hex `head`, and
  `base: "main"`.
- `blocker` and `next_action`: required when `status` is `BLOCKED`, so the
  output can be handed to an owner without an extra investigation.

Run against a recorded snapshot with a replayable clock:

```sh
python3 tools/graph/graph_stall_guard.py \
  path/to/graph-agent-status.json \
  --now 2026-11-08T10:00:00Z
```

The command prints a JSON verdict and writes an actionable `BLOCKED <lane>:`
line to stderr for every failed lane. Exit 0 means all lanes have current,
hash-matched evidence (or are explicitly complete); exit 1 means the snapshot
or at least one lane is blocked. The guard is read-only and performs no GitHub
or fleet mutation.

The guard intentionally does not infer runtime qualification from a green PR,
a local build, or a status document alone. A missing heartbeat, receipt, or
exact PR linkage remains blocked until the owner supplies that evidence.
