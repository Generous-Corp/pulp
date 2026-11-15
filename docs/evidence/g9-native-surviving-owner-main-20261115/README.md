# Native surviving-owner revalidation on current main

Evidence only; full G9 production custody remains **HOLD**, and so does
host-death handling. This directory re-executes the unchanged bounded fixture in
`test/fixtures/g9_native_surviving_owner/` against current `main`, because the
native retained `ChildProcess` implementation changed after the original packet
was recorded. It adds no runtime, API or fixture change.

Lineage:

- `g9-native-surviving-owner-20261110/` is the original nine-case packet, built
  and run at `ebded6b0a98e7353a13c4e74d1e9bc138dd6b342`. It stays immutable.
- `g9-native-surviving-owner-p2-20261110/` strengthens only the offline assessor
  (no `nonretained_wait`; census refusal leaves exactly one zombie W row) and
  proves it with copied-record controls. It stays immutable.
- This directory is a fresh native execution with the strengthened assessor on
  `abb85fd983becdfba064697826b34db21486e4e8`.

## Why a rerun was needed

Between the original base and this main, commit
`c47cd275b341050a0db9ec7794ca99f56e4237e1` (join native children to the current
process group) changed `child_process.hpp` and `child_process_posix.cpp`. The
change adds a joined mode and widens shared launch, destructor and cleanup
paths to cover it. Reading the diff, the retained private-group launch the
fixture uses still sets `POSIX_SPAWN_SETPGROUP` with group 0 and close-by-default
descriptors for a private input channel, but because shared code moved the old
receipts no longer describe this binary; only a rerun can say whether the
behavior held. The rebuilt executable
therefore no longer matches the original binary hash (117432 bytes, SHA-256
`d1cdaa7e...`); the rebuilt vendor module is byte-identical (35688 bytes,
SHA-256 `de297bea...`). `provenance.json` binds the new hashes, sources and raw
files.

The fixture C++ sources are clang-formatted in this tree. That change is
whitespace only (plus one CMake comment line marking the project as outside the script-inputs gate): a governed Release rebuild from the formatted sources produced
an executable and module byte-identical to the ones that ran, so the receipts
still describe this tree. `provenance.json` keeps both the as-executed and the
formatted source hashes. The original packet's provenance keeps its own
pre-format hashes, as recorded.

## Recorded outcome

One governed Release build (`-O3 -DNDEBUG` on all three compilations, see
`build.log`) and one native run per case, all nine passing:

| Case | Containment oracle | Cleanup call, ms | Original, ms |
| --- | --- | ---: | ---: |
| healthy | accepted | 0.406 | 2.019 |
| host EOF | accepted | 102.011 | 103.319 |
| host SIGKILL (H exit -9) | accepted | 102.037 | 102.758 |
| failed KILL / finite fork / retry | accepted | 113.121 | 115.516 |
| planted descriptor leak | rejected, as required | none | none |
| restored descriptor closure | accepted | 102.650 | 102.409 |
| injected census EIO | incomplete, W retained, A quarantined | 2000.373 | 2000.768 |
| anchor SIGSTOP | UNKNOWN from independent census | none | none |
| anchor SIGKILL | UNKNOWN from independent census | none | none |

Every verdict snapshot completed inside the five-second deadline; the longest
was 2.088 s (census refusal). No fixture process remained one second after the
run. Timing figures are single observations, not a scheduling or audio-timing
qualification.

The planted-leak case is the negative control for the containment oracle: the
same oracle that accepts the restored case rejects it. Separately, disabling the
native close-by-default flag in `child_process_posix.cpp` makes the restored
case fail; the land commit records that check with
`tools/scripts/confirm_failure.sh`.

## Not claimed

No production custody completion, escaping-descendant containment, host death
beyond the single direct SIGKILL case, real kernel signal-failure handling, fork
racing a successful KILL, Linux or Windows, installed-SDK or CLAP qualification.
Anchor failure state still comes from the independent census, never from H's
native snapshot.

## Reproduce

```sh
cmake -S test/fixtures/g9_native_surviving_owner -B build/g9-native-surviving-owner-main \
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.4 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PULP_BUILD_JOBS=4 tools/ci/governed-build.sh cmake --build build/g9-native-surviving-owner-main \
  --target g9-native-surviving-owner g9-native-hung-vendor --verbose
python3 -B test/fixtures/g9_native_surviving_owner/run.py \
  --binary build/g9-native-surviving-owner-main/g9-native-surviving-owner \
  --vendor build/g9-native-surviving-owner-main/libg9-native-hung-vendor.so \
  --output <fresh-directory>
```

Do not pass `--recheck` against the committed original packet: it rewrites that
packet's canonical receipts. Copy the raw directory to a scratch location first.

`run.py` now names `encoding="utf-8"` on its text I/O and its `ps` census, as
the text-encoding lint requires. The scorer functions it shares with the P2
proof (`assess`, `selected`, `one`) are unchanged. Its file hash therefore moved
from the reviewed `419648a8...` to `5df59c3a...`. The P2 directory keeps the
reviewed hash: running its `verify_repair.py` on this tree passes every case and
control and leaves the original packet unchanged, but rewrites the two
`source_sha256`/`new_assessor_sha256` fields in its own outputs to the new file
hash, so do not commit that rerun. With the edited observer, a second live run of
all nine cases passed and an offline recheck of `raw/` reproduces every recorded
assertion and containment verdict.
