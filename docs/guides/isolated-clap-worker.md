# Isolated CLAP worker

`load_isolated_clap` is an opt-in factory for experimental
`MacClapStereo64V1`. It returns an owned `PluginSlot` for the existing
`SignalGraph::add_plugin_node` and a retained `IsolatedClapControl` for lifecycle
operations. The parent hashes files without loading vendor code; the companion
uses the existing strict CLAP loader in a retained child process.

This mode requires macOS arm64, stereo main input/output, 48 kHz, exactly 64
callback frames and a headless CLAP effect reporting zero latency and tail.
Both latency and tail extensions must exist. Supply the exact descriptor ID,
companion path, plugin executable SHA-256 and companion SHA-256 through
`IsolatedClapOptions`. For a bundle, hash its executable under `Contents/MacOS`.
A missing or incompatible companion refuses creation. Other platforms refuse
explicitly.

The caller opts out of transport delivery. Descriptors cannot establish that a
vendor does not depend on transport. MIDI, automation, sidechains, editor,
double precision and offline process mode are unsupported. MIDI output containers
must arrive with empty SysEx/UMP sidecars; a nonempty sidecar refuses without
disposing caller-owned variable-length payloads on the callback. Serial canonical
graph execution is the admitted path. Anticipation, parallel execution, pinned
execution, live swaps and DSP-history carry are outside this mode.

## Callback contract

The parent creates one private 8192-byte shared mapping before launch. Both ends
check exact size and retained file identity. The pathname is unlinked after the
mapping handshake, before vendor loading. Three fixed request and three
completion slots carry fixed-width generation, sequence, dimensions and result
fields plus 512 audio bytes. Lock-free release/acquire atomics transfer exclusive
payload ownership.

Callback zero outputs silence and submits request zero. Later callbacks accept
only the preceding completion already published at entry, then submit the next
request. Normal graph PDC sees exactly 64 added samples. The callback uses
bounded copies, validation and atomics without waits, retries, allocation,
locks, logging, signals or lifecycle-channel calls.

Missing completions, full slots, malformed records, vendor errors and nonfinite
output permanently latch silence. Late results never move the stream. The
worker checks the real slot's `process_status()` before publishing success;
finite partial writes followed by `CLAP_PROCESS_ERROR` are discarded.

`IsolationStatus` snapshots retain the first terminal reason and block sequence,
generation/PID, lifecycle and cleanup outcome. Ordinary `ProcessStatus` remains
`ok` after terminal silence, allowing healthy graph siblings to continue. There
is no automatic restart or in-process fallback.

## Lifecycle and recovery

Retain both handles. Stop callbacks and drain graph readers before calling
`pause`, `checkpoint`, `stop` or `cleanup`; these are control-thread operations.
A slot may be prepared once. Load, prepare, restore, checkpoint and graceful
destruction each have five-second control deadlines.

Stopped checkpoint capture invokes native CLAP state saving through a writer
capped at 1 MiB. Any failed or oversized write invalidates the attempt even if
the vendor ignores failure, preserving the last accepted checkpoint. Accepted
state binds the plugin bytes/descriptor, fixed mode/rate/shape and state digest.
Recovery requires complete previous cleanup and constructs a fresh mapping,
worker and generation. It restores accepted state and primes silence; old
proxies remain terminal and uncheckpointed DSP history is lost.

The host reserves one of two finite entries **before spawn**, including failed
handshakes. `RetainedProcessCustody` preserves child identity through group
signaling. Cleanup gives TERM up to 100 ms, then KILL, with a two-second deadline
and no blocking final wait. Incomplete cleanup retains the process, mapping and
reservation and refuses new creation/recovery. `cleanup(deadline)` permits
explicit completion of a quarantine.

## Qualification limits

The test-owned hostile fixture is never installed as a normal plugin. Tests
exercise real lifecycle faults, bounded state, generation recovery, retained
descendants, expired-deadline reservations, callback protocol and sibling/PDC
behavior. Production mutations must fail their controls and pass after
restoration. Parent callback allocation/lock/syscall traps are functional
checks, not complete realtime qualification.

This is crash/hang containment while the parent survives, not a security
sandbox. Host death, deliberate group escape and an independent custody anchor
remain subsequent work. Other rates/blocks, events, latency/tail shapes, Linux,
Windows Job Objects, installed A10 saved-configuration qualification and the full
paced realtime matrix remain unqualified.

## Reproducing the controls

Build the production companion and focused controls through the governor:

```sh
tools/ci/governed-build.sh cmake --build build --target pulp-test-isolated-clap-worker pulp-test-isolated-clap-protocol
ctest --test-dir build -R '^Isolated CLAP' --output-on-failure
```

After all other builds stop, the test-owned mutation runner modifies one actual
production source at a time, records positive/broken/restored return codes and
source hashes, and restores the original source and executable:

```sh
python3 test/fixtures/isolated_clap/run_mutations.py --build build
```

After a complete Release build, `cmake-g9-isolated-clap-sdk-consumer` stages an
installed SDK and production companion into a fresh private prefix, copies the
public consumer into a separate source directory and runs half-gain,
checkpoint/recovery, generation and PDC controls. It compares installed public
header hashes and records SDK/worker/plugin/consumer provenance. This functional
consumer does not establish full A10 saved-configuration or realtime qualification.
