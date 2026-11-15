# Native surviving-owner experiment

Evidence only; full G9 production custody remains **HOLD**. This fixture composes
the merged retained `ChildProcess` API without changing its implementation or
adding a public API. It is not the first-mode worker, the native joined-child
prerequisite, a CLAP qualification, or an installed SDK consumer.

The independent observer starts H. H starts A through the native retained
private-input API; A uses that same API to start W in a distinct private process
group. A is W's actual parent and sole reaper. Only W loads the test-owned hung
module, which creates finite descendants. The native implementation is compiled
directly from this checkout, not copied or obtained from an unbound archive.

The retained leader prevents identity release during cleanup. Process census
is completion evidence, never signal authority. The observer never sends a
process-group signal. A census error keeps A's record quarantined and unreaped.
When A is stopped/killed, H retains authority over A only and reports UNKNOWN
for W; it cannot inherit W custody. Full production containment is not established.

The five-second absolute CLOCK_MONOTONIC deadline starts before H launch. Native
cleanup retains its existing two-second cap, 100 ms TERM grace and 10 ms KILL
retry spacing. Every outer verdict snapshot precedes hygiene. Vendor timeout
fuses and the subsequent resumption of a stopped A are hygiene, never success.

For the descriptor control, H sends an alias of its own channel endpoint to A
with SCM_RIGHTS. A duplicates it to descriptors 80/81, launches W, then closes
its aliases. Correct native close-by-default removes both from W. The planted
negative removes that spawn flag only in A's test-local syscall wrapper; the
resulting real W aliases can suppress A's observation of H EOF. This must fail
the same containment oracle that accepts the baseline/restored cases.

Other test-local wrappers observe the unchanged native waitid/waitpid, sysctl
and kill calls. The retry case injects one EAGAIN at the libc-call boundary and
cues one further vendor fork before the native retry. This is not a kernel
rejection or proof of fork racing successful signal delivery. The census-error
case injects EIO solely for the retained W group's census. Ordinary calls still
delegate to libc; positive receipts must demonstrate actual interception.

Source changes are limited to this evidence directory and
`test/fixtures/g9_native_surviving_owner/`. Runtime receipts and the final source
manifest are recorded alongside this document. Review the actual assertions and
raw chronology before promoting any claim.

## Recorded outcome

Fresh fetched `origin/main` base:
`ebded6b0a98e7353a13c4e74d1e9bc138dd6b342`. One native run per named case;
the separately named restored case is the required positive after the leak.
The first native witness completed 14m22s after assignment. No full gates,
production implementation, joined-child source, or first-mode source was used.

| Case | Expected result observed | Cleanup call, measured ms |
| --- | --- | ---: |
| healthy | Complete; A alive, W absent | 2.019 |
| host EOF | Complete; hung W and descendant gone | 103.319 |
| host SIGKILL | Complete; actual H exit -9 | 102.758 |
| failed KILL / finite fork / retry | Complete; actual fork before successful retry | 115.516 |
| planted descriptor leak | Same containment oracle rejects; W live after 400ms dwell | none |
| restored descriptor closure | Complete | 102.409 |
| injected census EIO | Incomplete, exited W identity retained, no reap, A quarantined | 2000.768 |
| anchor SIGSTOP | Independent census sees stopped A and live W; UNKNOWN | none |
| anchor SIGKILL | Independent census sees absent/zombie A and live W; UNKNOWN | none |

All verdict observations completed within the original five-second deadline;
the longest was 2.193088s. The nominal two-second native cleanup deadline is
not a hard scheduling guarantee: census refusal returned in 2.000768s including
return/logging overhead. Neither scheduling latency nor audio timing is qualified.

`raw/canonical.json` contains all nine final verdicts. `provenance.json` binds
the native header/source, fixture sources, compiler commands, binary/module
hashes, every raw artifact and canonical verifier hash. No SDK or prebuilt
archive was reused. `post-hygiene.json` records zero remaining fixture PIDs;
that later snapshot is explicitly not the containment oracle.

The original execution receipts remain immutable. The first witness used the
observer archived as `first-witness-observer.py.txt`; the remaining eight used
`runtime-observer.py.txt`. Before the later cases, the census assertion was
corrected to the actual native Exited enum value 3. Final review added baseline
live-W/live-A, timestamp ordering, actual H signal exit, and independent anchor
state assertions. The final `run.py --recheck` evaluates those stronger checks
against the saved raw observations without launching or signaling anything.
Both execution-source hashes and the final assessor hash are retained; the
final assessor is not represented as the program that generated earlier data.

## Focused reproduction

From the checkout root (new output directory required for a new execution):

```sh
cmake -S test/fixtures/g9_native_surviving_owner -B build/g9-native-surviving-owner \
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.4 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PULP_BUILD_JOBS=2 PULP_GOVERNED_BUILD_WAIT_SECS=1 tools/ci/governed-build.sh \
  cmake --build build/g9-native-surviving-owner \
  --target g9-native-surviving-owner g9-native-hung-vendor --verbose
python3 test/fixtures/g9_native_surviving_owner/run.py \
  --binary build/g9-native-surviving-owner/g9-native-surviving-owner \
  --vendor build/g9-native-surviving-owner/libg9-native-hung-vendor.so \
  --output <fresh-evidence-directory>
```

For the committed raw evidence, pass `--recheck` with its existing output
directory instead. The governor's documented two-job fallback was used after
capacity denial; no broader build or governor bypass occurred.
