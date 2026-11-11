# G12 activation receipt candidate

This candidate ports the six functional/analyzer/receipt source commits and
the preserved review-repair packet onto actual main
`ebded6b0a98e7353a13c4e74d1e9bc138dd6b342`. Its source files are byte-identical
to tested source `16cff8260d6c2b97f9c56d2c97deaf916c845e1e`.
The repair packet is byte-identical to evidence commit
`45343b1539a8597116b057175bc8b1e3ef752005`.
The original `e8f5ec890de483fc8882a198d7941d3078ac56ec` packet and original
RTSan checkout remain clean and unchanged in their original worktrees. The
original packet is not duplicated in this candidate. No preserved raw header
was formatted, renamed or removed from its original packet. The candidate's
normal formatting check passes.

Candidate source/evidence head before this packet:
`faf8d69a181a102455a70a40aa77e0da18838c76`.
Fresh native evidence in this directory targets that reachable source head;
the earlier packets remain historical evidence at their recorded heads.

The G12 receipt lane preserves the offline v2 40-cell contract. The paced lane
remains a bounded pilot, not timing qualification or G12 closure. No new
scheduler, production runtime/API, CMake, fleet policy or receipt tool exists.
The three owned source files are the activation fixture and two existing
qualification Python files. This directory and the unchanged imported repair
packet are the only evidence paths in the candidate diff.

## Policy gates

Local `gates.sh origin/main` exits 0 with four NOT CHECKED lanes. The real
pre-push hook exits 0 with diff coverage NOT CHECKED. This is the installed
`dedicated-builder` host policy, not a full gate claim. No skip variable,
artificial host profile, formatting demotion or hook bypass is used. Full CI,
independent exact-head review and any remaining configured checks must finish
before admission. The current goal authorizes one unarmed PR; it does not
authorize arm, enqueue, auto-merge or merge.

## Reserved-host setup correction

A read-only readiness probe found macmini idle, but I started an isolated clone
and Python dependency setup before reading the fleet handbook's explicit
reserved-host rule. That was an error. I stopped only my identified process
PIDs, verified no owned processes remained, and archived the clone with the
verified transfer bundle. No Pulp build or gate ran on macmini. Its primary
checkout stayed clean. Homebrew added the ca-certificates 2026-11-05 keg;
OpenSSL and Python were not installed. I did not remove shared dependencies or
continue work on the host. Raw setup, stop and cleanup records are retained.
No new full gate ran on m1 and no m5s slot was used. The m5 capacity probe
showed both slots occupied by live gate leases.

## Fresh candidate proof

At source `faf8d69a181a102455a70a40aa77e0da18838c76`:

- 14 Python tests pass, including all original offline v2 controls.
- 914 assertions in the three ordinary G12 cases pass.
- Actual CI backend positive/broken/restored proof passes: exits 0/42/0,
  with exactly one assertion failing for the planted routed-to-direct call.
  Positive/restored binary hashes are identical; the broken binary hash differs.
- 24 actual pilot cells x64 callbacks validate, totaling 1,536 callbacks and
  184,320 mono samples. There are **44 deadline misses**;
  `timing_qualified=false`. No quiet-host or WCET claim is made.
- Actual omitted-commit and wrong-gain runs fail on both serial backends;
  positive/restored runs pass (2/4/2). Ten re-signed receipt/raw negatives fail
  on a temporary copy; the original receipt and raw files stay unchanged.
- The pilot archive validates independently after extraction against its
  reachable source ancestor. `manifest.json` binds raw/source/binary/build data.

The emitted Git/build/dirty fields are configure-time stamps, not attestation
of every linked object. Real wrong-gain faults are rejected at missing expected
activation; the separate re-signed raw-audio control reaches the per-sample
oracle. Public prepared-buffer components and retained harness storage are
scoped resource counts, not full RSS/scratch/stack memory. RTSan evidence stays
at its separately preserved original tested heads; no new analyzer run is
claimed for this source-identical review successor.

The imported repair packet is historical evidence at its original heads and
requires those old Git objects for its original validator replay. Use this
candidate's new pilot for independent replay from the PR ancestry.

Full CI/configured checks, independent exact-head review, separately admitted
quiet qualification and broader G12/program obligations remain outstanding.
Only one unarmed PR is authorized. No runtime change, timing qualification,
arm, queue, auto-merge, merge or G12 closure is part of this handoff.
