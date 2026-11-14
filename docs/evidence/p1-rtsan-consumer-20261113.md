# P1 consumer realtime (RTSan) receipt, 2026-11-13

Issue 12177, item 2. Realtime proof of the P1 event-admission consumer's combined
audio-callback path under RealtimeSanitizer.

## Subject

- Base: `origin/main` `43ec0617b4c895437fd0183f483cf73d2a5a7f81` (P1 combined-epoch and
  executor-parity consumer merged).
- Source: `examples/p1-event-admission-consumer/main.cpp`, the same file the installed-SDK
  consumer builds. Its per-block audio side now lives in `combined_audio_block()`, which
  carries `[[clang::nonblocking]]` when `__has_feature(realtime_sanitizer)` and is a plain
  function otherwise.
- In-tree target: `pulp-test-p1-event-admission-rtsan`, registered by
  `test/cmake/p1_event_admission_rtsan_tests.cmake` only when the tree is configured with
  `PULP_SANITIZER=realtime`. It does not exist in the required gate's configure.

### What runs inside the boundary

Per block, inside `combined_audio_block()`:

1. `HostEventIngress<32>::project` into a preallocated `HostEventRecord` array, conversion to
   block-relative offsets into a reserved `MidiBuffer` with the realtime capacity limit set,
   `SignalGraph::inject_midi` into the MIDI input node, and `HostEventIngress::end_block`;
2. `SignalGraph::inject_parameter_events` into the `PluginSlot` node (parameter mailbox);
3. `SignalGraph::process` for the combined graph: input, graph-event lane producer and
   consumer (`CustomNodeType::process_events`, `GraphEventWriter::emit_at`,
   `GraphEventReader::events`), routed MIDI, the `PluginSlot` probe, output;
4. `VoiceBankBinding::begin_block`, `publish` for that block's voice records, and
   `VoiceBankBinding::render` (its own prepared `SignalGraph` and `MpeSynthVoice` processor).

Outside the boundary (control side): `HostEventIngress::begin_block` and the block-0 MIDI
staging `publish` calls, building the `ParameterEventQueue` batch, building the voice
`HostEventRecord`s, filling the input ramp, appending graph output to the run vector, slot
observation, and every `check()`. Result codes are recorded inside the boundary and checked
after it returns.

Every combined render goes through the boundary: the five combined schedules
(1, 32, 64, 127+127+2, 3+61+1+127+64) plus the executor-parity runs on the reference walk,
the routed serial executor and the parallel-after-bind request (refused, so serial). The binary
reports `realtime_boundary_blocks=1616`: 276 for the five primary schedules, 828 for the three
parity backends at those schedules, and 512 for the reference and routed runs that seed the
parity planted negatives at the 1-sample schedule.

### Planted controls

`P1_RTSAN_CONTROL` selects a real intercepted operation (compiled only under RTSan):

| Control | Where it runs |
| --- | --- |
| `malloc` | boundary entry, `malloc`/`free` through a volatile function pointer |
| `mutex` | boundary entry, `std::mutex::lock`/`unlock` |
| `sleep` | boundary entry, `std::this_thread::sleep_for(1ms)` |
| `node-malloc` | inside the combined graph's `CustomNodeType::process_events`, reached through `SignalGraph::process` |
| `voice-malloc` | inside `P1VoiceT::render_stereo`, reached through `VoiceBankBinding::render` |
| `clean` | nothing (explicit clean control) |

An unknown name fails with `rule=rtsan-control-name`. The two callee controls prove the
realtime context reaches through the graph executor and the voice bank's own graph, so a clean
positive run is not an artifact of the boundary covering only its own frame.

## Toolchain

- Host: arm64 macOS 27.2 (Darwin 27.2.0), shared (load about 22 on 28 cores).
- Compiler: Homebrew `clang version 21.1.8` at `/opt/homebrew/opt/llvm@21/bin`. Binary links
  `/opt/homebrew/opt/llvm@21/lib/clang/21/lib/darwin/libclang_rt.rtsan_osx_dynamic.dylib`.
- SDK: `/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk` (the 27.2 SDK breaks
  `<complex>` under Homebrew libc++; see `docs/evidence/g12-rtsan-receipt-20261113.md`).
- Whole tree instrumented: `pulp-host` and its dependencies compile with
  `-O3 -DNDEBUG -fsanitize=realtime -fno-omit-frame-pointer -g`; `CMAKE_BUILD_TYPE=Release`.

## Build

```
cmake -S . -B build-rtsan -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm@21/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@21/bin/clang++ \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk \
  -DPULP_SANITIZER=realtime -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=OFF \
  -DPULP_ENABLE_GPU=OFF -DPULP_TEST_PCH=OFF
PULP_BUILD_JOBS=4 nice tools/ci/governed-build.sh cmake --build build-rtsan \
  --target pulp-test-p1-event-admission-rtsan
ctest --test-dir build-rtsan -L rtsan -R p1-event-admission --output-on-failure
```

Build rc 0. The non-RTSan path of the same source was syntax-checked with Apple clang
(`/usr/bin/clang++ -fsyntax-only`, no sanitizer): rc 0.

## Results

Run from `build-rtsan/test`, binary `./pulp-test-p1-event-admission-rtsan`.

| Case | Command | Exit | Result |
| --- | --- | --- | --- |
| Positive | `./pulp-test-p1-event-admission-rtsan` | 0 | PASS, 0 RTSan reports, `realtime_boundary=nonblocking realtime_boundary_blocks=1616` |
| Clean control | `P1_RTSAN_CONTROL=clean ...` | 0 | PASS, 0 RTSan reports |
| Planted malloc | `P1_RTSAN_CONTROL=malloc ...` | 134 | DETECTED: `malloc` |
| Planted mutex | `P1_RTSAN_CONTROL=mutex ...` | 134 | DETECTED: `pthread_mutex_lock` via `std::mutex::lock()` |
| Planted sleep | `P1_RTSAN_CONTROL=sleep ...` | 134 | DETECTED: `nanosleep` via `std::this_thread::sleep_for` |
| Planted node-malloc | `P1_RTSAN_CONTROL=node-malloc ...` | 134 | DETECTED: `malloc` under `SignalGraph::process` |
| Planted voice-malloc | `P1_RTSAN_CONTROL=voice-malloc ...` | 134 | DETECTED: `malloc` under `VoiceBankBinding::render` |
| Unknown control | `P1_RTSAN_CONTROL=bogus ...` | 1 | `FAIL rule=rtsan-control-name` |

The positive run's functional output is unchanged from the merged consumer: checks 20137,
27 combined records, digests `midi:349d8d9cbaead7ac`, `parameter:21c6a5a653826137`,
`trigger:79300472c428c8e4`, `voice:76096164fc3782a6` (the receipt baseline), 15 parity
comparisons, routed executor 276/276 blocks, 0 routing fallbacks, parallel refused after bind.

CTest (`-L rtsan -R p1-event-admission`): 6/6 pass. The positive test passes on
`realtime_boundary=nonblocking realtime_boundary_blocks=[1-9]` and fails on any
`RealtimeSanitizer` output; each control test runs with `RTSAN_OPTIONS=abort_on_error=0` and
passes only on `RealtimeSanitizer: unsafe-library-call` followed by the expected intercepted
function. Wiring control: with `P1_RTSAN_CONTROL=malloc` and with `=node-malloc` inherited
into the positive test, it fails (`Subprocess aborted`).

No RTSan violation occurred in any positive case.

### Detection excerpts

`P1_RTSAN_CONTROL=node-malloc` (the realtime context reaches the routed executor):

```
==80814==ERROR: RealtimeSanitizer: unsafe-library-call
Intercepted call to real-time unsafe function `malloc` in real-time context!
    #0 ... in malloc+0x20 (libclang_rt.rtsan_osx_dynamic.dylib:arm64+0x34c4)
    #1 ... in ...render_combined(...)::$_0 ... operator()(...) function.h:174
    #2 ... in pulp::host::(anonymous namespace)::custom_binding(...) signal_graph_executor_routing.cpp:239
    #3 ... in pulp::format::(anonymous namespace)::run_routed_node(...) graph_runtime_executor.cpp:731
    #4 ... in pulp::format::GraphRuntimeExecutor::process_routed(...) graph_runtime_executor.cpp:1643
    #6 ... in pulp::host::SignalGraph::process_snapshot_impl(...) signal_graph_process.cpp:511
    #7 ... in pulp::host::SignalGraph::process_impl(...) signal_graph_process.cpp:55
    #8 ... in (anonymous namespace)::combined_audio_block(...) main.cpp:1142
```

`P1_RTSAN_CONTROL=voice-malloc` (the realtime context reaches the voice bank's graph):

```
Intercepted call to real-time unsafe function `malloc` in real-time context!
    #1 ... in (anonymous namespace)::P1VoiceT<4096u>::render_stereo(float*, float*, int) main.cpp:678
    #2 ... in pulp::format::Processor::process(...) processor.hpp:833
    #3 ... in pulp::format::ProcessorNode::process_main_bus_block(...) processor_node_adapter.cpp:269
    #8 ... in pulp::host::SignalGraph::process_impl(...) signal_graph_process.cpp:55
    #9 ... in pulp::host::VoiceBankBinding::render(...) voice_bank_binding.cpp:572
    #10 ... in (anonymous namespace)::combined_audio_block(...) main.cpp:1147
```

`P1_RTSAN_CONTROL=mutex` and `=sleep`:

```
Intercepted call to real-time unsafe function `pthread_mutex_lock` in real-time context!
    #1 ... in std::__1::mutex::lock()+0xc (libc++.1.dylib:arm64+0x3080)
    #2 ... in (anonymous namespace)::inject_analyzer_control(...) main.cpp:118
    #3 ... in (anonymous namespace)::combined_audio_block(...) main.cpp:1121

Intercepted call to real-time unsafe function `nanosleep` in real-time context!
    #1 ... in std::__1::this_thread::sleep_for(...)+0x50 (libc++.1.dylib:arm64+0x8e80)
    #2 ... in (anonymous namespace)::inject_analyzer_control(...) main.cpp:123
```

The malloc control reports `main.cpp:113` under `combined_audio_block` `main.cpp:1121`; the
voice-malloc report reaches `combined_audio_block` at `main.cpp:1147`. Line numbers are from
the committed source.

## Boundary of the claim

Local arm64 macOS, Release, LLVM 21 RTSan, N=1 run per case, one source head, built in-tree
from the consumer's own source against source-tree targets (not an installed SDK). It proves
that, for this fixture's graph and schedules, the combined audio path (MIDI projection and
`inject_midi`, `inject_parameter_events` into the `PluginSlot` node, `SignalGraph::process`
including the graph-authored event lane on both the reference walk and the routed serial
executor, and `VoiceBankBinding` publish and render) makes no call RTSan intercepts:
allocation, deallocation, mutex, sleep, or other blocking libc and system calls. RTSan does not
see bounded or unbounded spin loops, lock-free contention, or worst-case execution time, and
it only observes paths these schedules exercise. Not covered: parallel routing (refused once
the graph-event lane is bound), first-block lazy paths outside these schedules, `prepare()`,
topology edits, Linux or other toolchains, an installed-SDK RTSan build, and deadline or timing
qualification. The hosted `rtsan` workflow job remains dispatch-only.
