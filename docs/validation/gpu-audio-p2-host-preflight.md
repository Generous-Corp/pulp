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

## Required campaign inputs

A hardware campaign (anything but `--plan-only`) refuses to start unless it is
given all three of these:

- `--host-preflight PATH`: the host admission receipt described above.
- `--expected-source-revision SHA`: the 40-character Pulp commit the campaign is
  meant to measure. The campaign compares it with the checkout it runs from and
  fails if the measured source differs. The host-preflight's `source_revision`
  must name the same commit.
- `--provider-identity-receipt PATH`: a passed exact-provider receipt (schema
  `pulp.gpu-audio-provider-identity.v1`). It must be a regular file, not a
  symlink. Its `manifest_sha256` must equal the current
  `tools/deps/manifest.json`. Its expected and validated provider asset hashes
  must agree, as must its expected and header Dawn revisions. Every raw
  provenance row must then match the receipt's Dawn revision, provider asset,
  linked Dawn archive and manifest bindings.

The campaign reads the provider receipt once and records the SHA-256 of the
bytes it validated, with its path, in `campaign.json`.

```sh
python3 tools/scripts/gpu_audio_p2_campaign.py \
  --probe build/test/pulp-gpu-shared-io-paced-convolution-probe --output-dir /path/to/new/campaign \
  --host-preflight host-preflight.json \
  --expected-source-revision "$(git rev-parse HEAD)" \
  --provider-identity-receipt provider-identity.json
```
