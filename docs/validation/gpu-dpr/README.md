# A4 DPR v2 evidence

The canonical A4 result is
[`terminal-result.json`](terminal-result.json). It is intentionally
`inconclusive`, contains zero original and zero repeat cells, authorizes no
policy change, and leaves B5 waiting. That remains the truthful state until all
three canonical terminal dependencies exist as ordinary Git blobs on live,
protected Pulp `main`:

- `docs/validation/gpu-vellum-adoption/a2t-pulp-trace-analysis.json`
- `docs/validation/gpu-vellum-adoption/a3-pulp-dpr-product-policy.json`
- `docs/validation/gpu-vellum-adoption/a3-pulp-runtime-control.json`

Historical v1 runs and the retained `m3-*` records are nonterminal. They count
as zero v2 cells and cannot authorize collection, a disposition, publication,
or B5.

## Collection authority

Start v2 only from a clean checkout whose `HEAD` is the live protected-main
head and whose required checks are green. The runner reads the fixed dependency
paths above, validates their terminal envelopes and cross-receipt chain,
derives the authorized corpus from
`docs/contracts/gpu-dpr-corpus-v2-template.json`, and snapshots the exact A2T
analyzer. It creates the run directory itself; the path must be absolute and
absent:

```bash
python3 tools/scripts/gpu_dpr_runner.py init-v2 \
  --run-dir /absolute/owned/a4-dpr-run \
  --experiment-id <campaign-id> \
  --trace-analyzer /absolute/path/to/the/a2t-authorized-analyzer
```

There is no v2 `--manifest`, `--plan`, or caller-draft input. The local HMAC
detects accidental journal changes only. A user who can replace that key has
not created evidence: finalization rederives every cell from runner-issued
nonces, snapshotted adapter/producer receipts, retained artifacts, and the live
terminal dependencies.

## Exact 84 plus 84 corpus

Run each of the 168 canonical selectors with its real executable measurement
adapter; `status-v2` reports the remaining/completed counts:

```bash
python3 tools/scripts/gpu_dpr_runner.py run-v2 \
  --run-dir /absolute/owned/a4-dpr-run \
  --cell '<original-or-repeat>__<scenario>__<mode>__dpr-<value>' \
  --adapter /absolute/path/to/executable-adapter

python3 tools/scripts/gpu_dpr_runner.py status-v2 \
  --run-dir /absolute/owned/a4-dpr-run
```

Each accepted cell retains eight unique runner-owned regular files: raw trials,
frame sequences, measured capture, reference capture, trace, input receipt,
identity receipt, and executable product. The runner hashes and snapshots the
held source files, rejects symlinks/outside paths/hard-link reuse, validates
PNG pixels and dimensions, reruns the A2T trace analyzer, checks the executable
file format, and binds all bytes to the exact cell, nonce, trials, producer and
product processes, adapter, build, provider, host, app, and format. Adapter
stdout and stderr are independently capped at 1 MiB. A timeout, output overflow,
bad exit contract, missing receipt, or rejected artifact closes that nonce as
inconclusive and permits a fresh attempt; it never completes the cell.

`finalize-v2` accepts only the authenticated run directory. It takes no result,
manifest, disposition, or draft from the caller:

```bash
python3 tools/scripts/gpu_dpr_runner.py finalize-v2 \
  --run-dir /absolute/owned/a4-dpr-run
```

Finalization freshly validates the live A2T/A3 receipts, derives the manifest
again, and replays all 168 accepted receipts plus 1,344 retained artifacts. The
run retains its initialization-time `authorized-manifest-v2.json` and the
finalizer writes `result-v2.json` under the run root. Those are candidate bytes,
not proof that they were published.

## Protected-main publication

Publish the runner-derived manifest and result bytes only at their fixed paths:

- `test/fixtures/gpu-ux/dpr/manifest.json`
- `docs/validation/gpu-dpr/terminal-result.json`

After those exact ordinary blobs are merged and required checks are green, use
a clean checkout at the live protected head to emit a durable receipt into the
same evidence root:

```bash
python3 tools/scripts/gpu_dpr_runner.py verify-live-v2 \
  --evidence-root /absolute/owned/a4-dpr-run

python3 tools/scripts/gpu_dpr_experiment.py validate-result \
  docs/validation/gpu-dpr/terminal-result.json \
  --evidence-root /absolute/owned/a4-dpr-run
```

Live verification rejects dirty, symlinked, outside, wrong-path, wrong-type,
wrong-blob, wrong-byte, substituted, or stale-head candidates. It binds the
fixed manifest/result paths, exact Git blob IDs and bytes, live protected head,
and exact required-check identities. The final validator recomputes that state
and requires the durable receipt to match; booleans written into the result are
never publication authority.
