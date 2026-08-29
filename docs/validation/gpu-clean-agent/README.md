# GPU clean-agent journey

This is the Horizon-A A5 usability gate. It proves a fresh investigator can
start with an exact symptom token and complete the workflow without reading the
Pulp source tree or remembering a recipe ID:

1. `pulp gpu recipes list --symptom ... --json` selects exactly one callable
   recipe from the CLI's embedded canonical catalog.
2. `pulp gpu recipes scaffold ...` creates the bounded evidence workspace and
   records the selection.
3. An unmutated reference run binds the selected native recipe's exact ordered
   semantic passes, artifact identities, adapter, source, and signature.
4. The selected recipe's real `pulp gpu probe` command runs with its seeded
   negative control. Exit 1 must carry one completed, typed failing pass and
   hash-declared artifacts, with no unavailable or unverified pass hidden by
   the aggregate failure.
5. The agent applies the scaffold-documented correction: remove only
   `--negative-control`, leaving the input and independent oracle unchanged.
6. The same recipe reruns successfully. Its typed receipt must prove work
   completed, reproduce the reference pass/artifact/source/signature contract,
   and reproduce every non-output reference artifact exactly. Every artifact
   changed by the seeded mutation is named explicitly rather than inferred from
   its filename.

Run it against a built or installed CLI from an arbitrary directory. The
workspace must be an absolute path that does not exist, with a real existing
parent (not a symlink):

```sh
mkdir -p /private/tmp/pulp-gpu-agent-proof
python3 /path/to/pulp/tools/scripts/gpu_clean_agent_journey.py \
  --pulp /absolute/path/to/pulp \
  --symptom compute-readback-mismatch \
  --workspace /private/tmp/pulp-gpu-agent-proof/journey \
  --json
```

The command writes `clean-agent-journey.json`, all three complete typed probe
JSON results, and bounded `reference/`, `seeded-failure/`, and `repaired/`
artifacts beneath the
scaffold. The receipt records the live catalog revision, selected recipe,
the exact scaffold selection/README byte hashes and documented commands,
failure mutation/pass/code, applied fix, all evidence IDs and source/signature
digests, every artifact digest, all stable reference artifacts, changed output
artifacts, raw-result digests, and the CLI binary digest. Unknown, ambiguous,
conditional, unavailable, timed-out,
untyped, unbounded, or tampered evidence fails closed. Unavailable or
unverified evidence exits 2; a broken journey contract exits 1.

CTest runs the same process-level gate as `cli-gpu-clean-agent-journey`. The
checked-in `m3-a5-clean-agent-20260828.json` is one real M3 Ultra execution
receipt; it is evidence for this host and binary only, not a cross-platform
claim.
