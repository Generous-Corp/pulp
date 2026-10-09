# P2 host-preflight input

`tools/scripts/gpu_audio_p2_campaign.py` requires a fresh host admission
receipt before it launches a provider-backed campaign. This keeps a quiet-host
failure from being hidden by a successful numerical probe.

The receipt is JSON with schema `pulp.gpu-audio.p2.host-preflight.v1` and must
contain:

- `status: "passed"` and `quiet_host: true`;
- `source_revision` equal to the exact protected Pulp commit being measured;
- a non-empty `host_id` and ISO-8601 `sampled_at` no more than 900 seconds old;
- `host_vitals_level: "green"` (including the current
  `tools/scripts/host_vitals.sh --json` result);
- `gpu_contention: false`, `ui_contention: false`, and a non-empty
  `thermal_state`.

The campaign records the preflight's SHA-256, path, and host identity in
`campaign.json`. Missing, stale, source-mismatched, contended, or malformed
preflight input fails before the negative control or any provider submission.
The preflight is an admission check, not a performance verdict; the campaign
still remains `performance_verdict: "unassigned"` until its independent review.
