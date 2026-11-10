# G9 MacClapStereo64V1 implementation validation

Implementation admission is the planning-owned
`graph-program-status/g9-worker-isolation-admission-20261110.md`. The first
mode contains worker faults while the parent survives, covering descendants
remaining in the retained process group. It does not establish a security
sandbox, host-death custody, broader modes/platforms, full realtime or A10
qualification.

The host uses the separately owned retained-custody dependency, developed at
`4532d49b5e37a1c9cbe9633bb37641b220ce78cb`. Independent Opus technical recheck
accepted that head under the admitted custody contract. Development integration
is a normal merge with rerere disabled; dependency publication/admission remains
separate and must precede a main-based host PR. Its unarmed prerequisite is
[the retained-custody PR](https://github.com/Generous-Corp/pulp/pull/11996),
published at `09b3b3217421fa77ac4f22854aec9c9bd137868d`.

## Functional controls

Governed Release/Ninja build, examples and GPU off. The complete configured
all-target build passed again after the final mutations and byte restoration. The pinned planning submodule is initialized at
`baf40625f4ccfb1f4ab35b880762b4685839727c`, but does not contain the generated
importer corpus: those unrelated importer targets remain unavailable.

- Real worker suite: 146,247 assertions, 20 cases passed. Healthy/stateful
  gain, 1,024 changing-audio cross-process publications, lifecycle crash/hang
  deadlines, partial errors/nonfinite audio, exact prior-block audio, explicit
  padded raw lengths, nested descendants/exited leader, capacity two and two
  expired-deadline quarantines, bounded checkpoints/recovery, bundle resources,
  mismatched build, graph policy and hosted/built-in sibling/PDC controls.
- Existing host graph suite: 12,416 assertions, 144 cases passed.
- Repaired dependency's retained-name focused controls: 232 assertions, 11
  cases passed. The intentional custody-loss lane prints an unresolved-custody
  diagnostic; it is not promoted to cleanup success.
- Adversarial production callback endpoint: 1,044 assertions in one case passed: stale generation,
  wrong sequence/status/dimensions, missing completion, queue full, NaN and
  healthy publication.
- Installed public consumer passed in a fresh prefix and separate source tree:
  public factory, ordinary `add_plugin_node`, half-gain oracle, checkpoint/new
  generation recovery and 64-sample PDC. The final clean-head receipt reports
  `source_git_dirty=false` and exact source SHA; receipts include actual 8-block
  float32 input/output hashes (block-major, channel-major, native arm64 bytes),
  installed host library, public header, companion, plugin and consumer hashes.
- Structured autoreview accepted three concrete defects, repaired with real
  controls: bundle-root initialization, stereo graph admission before worker
  prepare, and SysEx/UMP sidecar refusal. Final production and additional
  failed-construction probe reviews returned no actionable findings.

Functional tests intentionally allow scheduling slack. Callback allocation,
mutex and syscall traps and changing-audio publication checks do not prove the
full paced realtime matrix. No 100,000-callback or unsupported rate/block claim
is made.

## Mutation controls

`test/fixtures/isolated_clap/run_mutations.py` mutates actual production sources,
requires an original positive, a broken failure and a restored positive, and
restores original bytes in `finally`. It records source hashes and numeric exit
codes under the selected build directory. Controls cover worker-status bypass,
parent generation/sequence bypass, ordinary `ProcessStatus::failed` sibling
regression, disabled descendant cleanup, a callback wait, and failed-construction
reservation retention. The last control proves normal failed-handshake cleanup first, then injects a
host construction failure after successful launch with an actually expired
cleanup deadline. Its fixture worker deliberately exits. Discarding that same
reservation must lose quarantine refusal. It creates no hostile descendants.

The final six mutation controls passed. Every production mutation built
successfully; original and restored executions returned zero. Broken results:

| Control | Original | Broken | Restored |
|---|---:|---:|---:|
| Worker ordinary-status check | 0 | 42 | 0 |
| Proxy ordinary-failed sibling regression | 0 | 42 | 0 |
| Retained descendant cleanup | 0 | 42 | 0 |
| Callback wait | 0 | -6 (RT trap) | 0 |
| Parent generation/sequence validation | 0 | 42 | 0 |
| Failed-construction reservation | 0 | 42 | 0 |

The reservation control additionally passed normal failed-handshake creation
and final original-source creation (both zero). The disabled-cleanup case
observed a live descendant, then cleaned the fixture before failing. Sources
were verified byte-for-byte restored against the runner's SHA-256 records:

- Worker: `83516c907ddd7bd551238f0c05ec5b49ff5a9c1d3c1ee1e59101a72a3a7a7a36`.
- Host: `af7c9a151d3932a38ea42689a1a0e894b6d24a64ae20c411d115cfdb5eb9b544`.
- Protocol: `53fb7459adb394fff76887070ae536b3a5792abe2f636ded09c047f3e2a22961`.

The exact-head full governed Release build and clean installed-consumer
provenance passed. Full gates remain pending dependency landing and actual
main reconciliation. Publication stays
unarmed and depends on the actual reviewed custody landing; no duplicate
main-based dependency PR is authorized.

## Integration closeout prerequisites

Generated capability rederivation waits for the required actual dependency/main
reconciliation. Full source-selftest gates must run on a permitted light host;
the local dedicated-builder host refuses that lane honestly.

The test-input generator also currently refuses unchanged existing
`test/support/control_sample_region_e2e.hpp` because its quoted include
`control_broker_daemon.hpp` is under `inspect/src`. Reproduce with
`python3 tools/scripts/script_test_inputs.py --build-dir build --write` after
fresh Release configure and JSX runtime npm dependencies. No inventory filtering,
handwritten generated rows or unrelated scanner/control rewrite was used.

A clean-head rerun at `53b882576306d5bfbe98bf9ab3b5d1afba86ae6c`
passed the installed consumer (`source_git_dirty=false`) but hit the correct
terminal deadline response at publication 974/1024 in the changing-audio
stress with 5ms functional pacing. The other 19 cases passed. The shared m3
host's measured load was 80.61/40.86/38.23. This failed timing observation is
retained; the data-publication functional control now uses explicit 20ms
slack. Neither functional pass nor that slack establishes paced realtime
admission or a guarantee on an oversubscribed host.
