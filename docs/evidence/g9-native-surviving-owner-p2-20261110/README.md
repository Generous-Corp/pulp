# Native evidence P2 repair

Successor to preserved `2c1e6201feb2d26d980c71457f5865825848511b`.
Independent review session: `390fd0b1-99b1-43a9-b8be-01d6e95e0c2e`.
The review accepted the bounded experiment with no P1 findings. Production
custody remains **HOLD**, with every original exclusion retained.

Only the assessor changes: any `nonretained_wait` witness rejects the record;
the census-error case also requires exactly one independent group row, for W,
in zombie state. Native C++, module, launcher and APIs are unchanged.

The original evidence directory, execution observers and source checkpoint
remain immutable. This directory owns the additional offline assessments,
copied-record negative controls and a fresh-directory governed Release rebuild.
No process fixture or signal experiment is rerun.

## Results

- All nine saved cases pass the stronger assessor.
- Six synthetic copied-record controls each produce old PASS / new FAIL /
  restored PASS: A and non-A `nonretained_wait`, missing W, live W, an extra
  live group member, and an extra zombie group member. Only the intended new
  assertion fails. Native self-reports remain unchanged in the census controls.
- All 109 files in the original packet retain their original hashes. Its
  canonical receipts, execution receipts and archived observers are untouched.
- A governed focused Release build in the new `build/g9-native-p2-fresh`
  directory compiled the same committed C++/header/CMake inputs. All three
  compilation commands contain `-O3 -DNDEBUG`. No SDK/archive reuse occurred.
- The rebuilt executable is 117432 bytes, SHA-256
  `d1cdaa7e3d20461eb216cde32befc61b73d47c2ef158b49277ddf95e5b8ed9aa`.
  The rebuilt module is 35688 bytes, SHA-256
  `de297beadf6dad063f2ef8c464762e775869d4a291735d33f918ec558377a996`.
  Both hashes and sizes exactly match the assets bound to the original runs.

`canonical.json` records the new assessment; `controls.json` records every
positive/broken/restored score. Each `copies/` record is explicitly synthetic:
it contains changed assessor arguments plus the immutable base case and context
hash. Materializing the overrides onto a deep copy reproduces its recorded
copy hash. Restoring the base context reproduces the positive hash. These are
scorer controls, not new process observations.

`rebuild.json` binds the fresh compile commands, unchanged source inputs, build
logs and artifact comparison. `review-binding.json` binds the exact independent
review, old/new assessor hashes and proof driver. `immutability.json` lists all
original packet hashes. `manifest.json` binds the final repair source and files.

Reproduce the offline assessment from the checkout root:

```sh
python3 -B docs/evidence/g9-native-surviving-owner-p2-20261110/verify_repair.py
```

The driver compiles only the three scorer function ASTs. It does not import
the fixture runner or invoke its launch, census or signal functions. Its Git
reads obtain the old assessor from the exact preserved commit.

The new build used the existing standalone CMake project with Ninja, Release,
macOS deployment target 13.4 and export-compile-commands enabled, followed by:

```sh
PULP_BUILD_JOBS=2 PULP_GOVERNED_BUILD_WAIT_SECS=1 tools/ci/governed-build.sh \
  cmake --build build/g9-native-p2-fresh \
  --target g9-native-surviving-owner g9-native-hung-vendor --verbose
```

No production custody completion, escaping-descendant containment, real kernel
signal-failure handling, fork racing successful KILL, Linux, scheduling/audio
timing, installed-SDK or CLAP qualification is claimed. Anchor failure state
continues to come from the independent census, not H's racing native snapshot.
No full gates, push, PR or runtime/API change is part of this repair.
