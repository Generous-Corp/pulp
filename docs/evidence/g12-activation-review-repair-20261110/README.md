# G12 activation receipt review repair

Status: bounded review-repair proof PASS; final gates INCOMPLETE/FAILED.
G12 remains PARTIAL; timing qualification is not run. No PR/push/merge.

Review addressed: `graph-program-status/g12-e8f5-independent-claude-review-20261110.md`
in the planning-graph-recovery-sync-20261110 checkout, independent session
`781ebe6b-d955-4e3a-8388-f50774b9bad8`. All three P2 findings and both P3
controls are within this successor's scope.

Final source: `16cff8260d6c2b97f9c56d2c97deaf916c845e1e`.
Backend positive/broken/restored proof was built at source
`301cff55ca3a1aed6cd7afca9cd6c31f387a8ba1`; the later source commit changes
only Python text encodings. The fixture bytes are identical at both commits.
Final native/pilot evidence is recorded separately against the final source.

The original source/evidence packet at `e8f5ec890de483fc8882a198d7941d3078ac56ec`
and RTSan source/evidence at `8f3e3b06e38be208afed9dc03a7778aa5f36861d`
are preserved byte-for-byte. This successor changes only the activation fixture,
qualification tool/tests, and this evidence directory. It adds ordinary CI
assertions for actual executor blocks, parallel dispatch, fallback and failures.
The planned broken control changes the ordinary test's routed invocation to
use direct execution, then rebuilds and requires a failure in those assertions.
Restoring the source must restore a passing run.

Receipt corruption controls operate on a temporary copy, including raw files.
An interrupted-copy unit test verifies the retained packet is untouched even
when a planted mutation raises. The miss mutation is a different nonnegative
count within the callback count, requiring comparison against raw callback
records rather than rejection solely for an invalid negative integer.

## Corrections to the preserved report

The `compiled_source`, `build_type` and `build_dirty` fields come from the
existing generated header at CMake configure time. They are compiled into the
fixture, but they are not a cryptographic attestation of all linked objects or
of the working tree at compile time. Their validation rejects mismatching
configure-time stamps; it does not prove an executable was built from every
claimed source byte. Retained fixture/compile-command/binary hashes and actual
configure/build logs provide separate provenance evidence, with that limitation.

The real wrong-gain controls in the old packet produce 0.375 instead of the
expected new 0.5 output. Both Catch and the independent parser reject them at
the missing expected activation check, because no block is classified New.
They therefore prove rejection of the real wrong-gain output, but do not prove
that those real runs reached the Python every-sample comparison. Separate
re-signed raw-audio corruption controls retain an expected activation and
exercise the per-sample oracle. The original raw logs remain authoritative.

## Scope and outstanding obligations

No scheduler, production/runtime/API, CMake, fleet or CI policy changes.
The 40-cell offline v2 receipt contract is preserved. Short pilot measurements
on this shared host establish no timing qualification or WCET bound. The
100,000-callback matrix requires a separately admitted quiet-host window.
Ordinary gates and hooks are run without bypasses; host-policy NOT CHECKED
lanes remain obligations before a final gate claim or PR.


## Gate disposition

`gates.log`: exit 4 because a merge-tree capability-manifest check timed out.
`gates-retry.log`: reached static gates; failed on missing explicit UTF-8 in
the owned tools, subsequently fixed in the final source commit. It also lists
source-selftests and changed-surface script-family validation as NOT CHECKED.
The configured host role is `dedicated-builder`; no full gate was started on
m1, and no m5s slot was used.

`pre-push.log` was obtained by invoking the real hook with empty ref input;
it checks local policy and does not push a ref or open a PR. It flags
`docs/evidence/g12-activation-receipt-20261110/compiled-build-info.hpp` as
unformatted. That is retained raw generated-header evidence in the original
packet. It has not been rewritten, renamed, removed or re-signed. No hook
bypass, formatting demotion, artificial host profile or policy change is used.
A future PR candidate can port the source commits and this new packet onto
current main without including the original packet, while leaving that exact
original packet at its preserved commit/worktree; otherwise its raw-header
formatting disposition requires separate admission. No PR is opened here.


## Final bounded results

- Fourteen Python tests pass, preserving offline v2 controls and the 40-cell contract.
- Final ordinary G12 tests pass: 914 assertions in three cases.
- Actual CI dispatch control: positive and restored exit 0; wrong-backend binary
  exits 42 with exactly the executor-block assertion failing (0 versus 128).
  The ordinary fixture file hash is identical at the backend-proof and final
  source commits. The planted source patch and all three binary hashes/sizes
  are retained in `backend-results.json`; the broken binary is not attributed
  to a clean source commit.
- Final actual pilot: 24 cells, 64 callbacks each, 1,536 callbacks and 184,320
  mono samples independently validated. There are **47 deadline misses**;
  `timing_qualified=false`. No timing threshold or WCET claim follows.
- Actual omitted-commit/wrong-gain controls fail on both serial backends;
  positives/restored runs pass (two positive, four broken, two restored).
- Ten re-signed receipt/raw corruptions are rejected on a copy. The original
  retained packet stays unchanged; the unit test additionally plants an
  interruption while the copy is corrupted.
- The retained pilot archive independently validates after extraction.

`pilot-bundle.tar.gz` retains raw stdout/stderr, exact fixture source,
compile commands, receipt and executable. `manifest.json` records sizes and
SHA-256 hashes of this packet, raw logs, source and final local binary/libraries.
`scope-check.json` verifies both original worktrees are clean at their exact
preserved heads and the original evidence directory has no diff.

The supported RTSan proof remains the separately preserved original/e8f5
packets at their recorded tested heads. No new analyzer run is claimed for
this review-only successor; its five native assertions are outside the
unchanged full graph callback boundary. This successor adds no runtime change.

To reproduce final bounded proof at the final source commit:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BUILD_EXAMPLES=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
tools/ci/governed-build.sh cmake --build build \
  --target pulp-test-group-trigger-graph --verbose
python3 -m unittest discover -s tools -p test_graph_performance_qualification.py
build/test/pulp-test-group-trigger-graph '[g12]~[.]'
python3 tools/graph_performance_qualification.py record-activation \
  --source-root . --build-dir build \
  --executable build/test/pulp-test-group-trigger-graph \
  --output-dir build/g12-review-final-pilot --callbacks 64 --timeout-seconds 30
python3 docs/evidence/g12-activation-review-repair-20261110/bounded-proof.py
```

`backend-proof.py` temporarily changes only the ordinary test invocation to
`run(false)`, rebuilds with the governor, requires that backend assertion to
fail, then restores the fixture and rebuilds in a finally block. Run it only
when no other build is using the checkout. It is a test-source mutation,
not a production runtime mutation.

Remaining: complete ordinary full gates/diff coverage on an admitted available
host/VM; resolve raw-header formatting for the candidate without altering the
preserved original packet; independent exact-head review; one unarmed PR only
after the final gates; separately admitted quiet-host qualification and all
remaining G12/program review obligations. No bypasses were used.

Raw configure/build/test logs are retained byte-for-byte, including compiler
whitespace and final blank lines. `git diff --check` is clean for owned source;
it reports whitespace in raw evidence logs, which has not been normalized.
The final binary is 32,238,464 bytes, SHA-256
`502d9367f958d9874821a98cab73cdbe31b362dfd215547d6c3d5738b21d1044`.
The pilot archive is 12,674,423 bytes, SHA-256
`f19d1bc103c741b49b76e8681fdc2b629270a8eb75ecfa3f37e15a43935f3e31`.

Final source gate result: `gates-final-source.log` exits 1 for the preserved
raw-header formatting failure. The corrected encoding checks pass. Full
source-selftests and configured script-family validation remain NOT CHECKED.
