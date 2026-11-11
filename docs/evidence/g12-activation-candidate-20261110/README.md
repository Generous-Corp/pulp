# G12 activation receipt candidate

This packet records bounded G12 activation receipt metadata for the source
commit `e893f20d0048dfc11536c4b688d11be97ba9e167`. That commit is reachable
from both `origin/main` and this pull request's checked-out head. The three
recorded source files are byte-identical to that commit; the source tree ID is
`a9a0cfc1a2a02844f6bfb69be249812ee15d8e1e`.

Run the explicit source binding check from the repository root:

```text
python3 docs/evidence/g12-activation-candidate-20261110/source-ancestry-check.py
```

The check verifies all of the following before writing
`source-ancestry-check.json`:

* the recorded source commit exists locally;
* it is an ancestor of `origin/main` and the checked-out head; and
* every recorded source-file digest matches both the working tree and the
  source commit.

## Receipt metadata

The retained JSON files contain only reproducible receipt metadata:

* `backend-results.json` records positive, planted-broken, and restored exit
  statuses plus content digests;
* `real-fixture-controls.json` records the direct and routed semantic controls;
* `pilot-summary.json` records 24 bounded cells, 1,536 callbacks, 184,320
  samples, and 44 deadline misses (`timing_qualified=false`); and
* `verification-results.json` summarizes the receipt controls and their
  boundaries.

Commands, local paths, process identifiers, lease details, host names,
capacity probes, build logs, and binary archives are intentionally omitted.
They are operational records and are not needed to reproduce the validator
contract. The manifest covers only this sanitized packet and the source files
used by the validator.

This remains evidence-only. The paced lane is a bounded pilot, not timing
qualification or G12 closure. No scheduler, production runtime/API, CMake,
fleet policy, or receipt-tool change is included. Full CI, independent review,
quiet-host qualification, and broader G12 obligations remain outstanding.
