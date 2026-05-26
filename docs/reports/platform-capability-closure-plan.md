# Platform Capability Closure Plan

**Date:** 2026-05-25
**Status:** Active closure pass
**Scope:** Runtime, events, audio I/O, audio formats, window embedding, OSC

## Summary

Pulp already has real implementation in each audited capability area, but the
support is uneven. The strongest foundations are child-process launching,
thread-backed task dispatch, platform audio I/O backends, WAV/AIFF/FLAC/MP3/Ogg
read paths, and OSC message transport. The current closure pass is intentionally
limited to threads/processes, native main-thread dispatch, OSC, and native window
embedding; audio format and device-manager gaps remain documented here for the
larger follow-up audit.

This plan is code-grounded. It treats existing implementation files as the
source of truth and avoids making product claims based only on planning docs.

## Execution Model

This report is the living tracker for the first closure pass. Each feature
track should update this file in its own branch with the final scope,
implementation notes, tests, coverage proof, and PR link before shipping.

| Track | Branch target | Worktree target | Status | Done means |
| --- | --- | --- | --- | --- |
| Threads and processes | `feature/platform-threads-processes` | `pulp-platform-threads-processes` | Merged via PR #2815 | Canonical platform process surface, runtime blocking wrapper, tested launch/wait/cancel/output/IPC behavior, no unneeded current-process or timer additions |
| Native event loop | `feature/platform-main-thread-dispatch` | `pulp-platform-main-thread-dispatch` | Merged via PR [#2825](https://github.com/danielraffel/pulp/pull/2825) as `9c96f3dfa` | Cross-platform main-thread dispatcher contract, platform registrations where available, sync/async dispatch tests, EventLoop thread-id race fixed |
| OSC | `feature/platform-osc` | `pulp-platform-osc` | PR [#2822](https://github.com/danielraffel/pulp/pull/2822) open; rebased and locally validated on `origin/main` `a7e223f8` after main consumed SDK `0.250.0`; SDK version is `0.251.0`; hosted Ubuntu install-consumer smoke passed on the SheenBidi install fix | Typed bundle send/receive, listener filtering using existing address matching, invalid-packet error callback, exclusive UDP receiver binding, focused UDP and pure parser tests |
| Native windows | `feature/platform-native-window-embedding` | `pulp-platform-native-window-embedding` | PR [#2844](https://github.com/danielraffel/pulp/pull/2844) open; locally rebased/validated; final SDK bump and push pending after #2822 | First-party non-Apple host/plugin embedding path or explicit supported-platform contract, child attach/bounds/detach tests, docs updated to avoid overclaiming |

Validation expectations for each PR:
- Add or update focused unit tests for every new public behavior.
- Run the smallest relevant CTest target during iteration, then broader
  validation before shipping.
- Run local diff coverage via `tools/scripts/local_diff_cover.sh` when the PR
  changes coverage-bearing C/C++ sources. The CI diff-coverage floor is 75%.
- Sweep the full local diff for correctness, lifecycle, cross-platform, and
  docs issues before opening the PR; use Claude as an independent review pass
  and fix actionable findings before submitting.
- Use the normal Shipyard/GitHub PR flow so required checks and Codecov
  comments are recorded before merge. For this focused pass, do not run SSH
  Windows/Ubuntu validation lanes; use local/macOS evidence and GitHub-hosted
  checks.
- After each PR opens, sweep Shipyard/GitHub review and CI comments, address
  actionable feedback on the same branch, and re-run focused validation before
  moving to the next feature.
- Keep each feature branch independently reviewable; do not mix unrelated
  cleanup from other tracks.

## Current State

| Area | Current status | Evidence |
| --- | --- | --- |
| Threads and processes | Partially implemented | `core/events/include/pulp/events/event_loop.hpp`, `core/platform/include/pulp/platform/child_process.hpp`, `core/runtime/include/pulp/runtime/child_process.hpp`, `core/events/include/pulp/events/child_process_manager.hpp`, `tools/scan-worker/CMakeLists.txt` |
| Native event loop | Partially implemented | `core/events/src/event_loop.cpp`, `core/view/include/pulp/view/window_host.hpp`, `core/view/src/window_host_stub.cpp` |
| Audio file formats | Partially implemented | `core/audio/src/format_registry.cpp`, `core/audio/src/audio_file.cpp`, `core/audio/src/aiff_reader.cpp`, `core/audio/src/ogg_reader.cpp`, `core/audio/src/flac_writer.cpp`, `core/audio/src/mp3_writer.cpp`, `core/audio/CMakeLists.txt` |
| Audio devices and multichannel audio | Partially implemented | `core/audio/include/pulp/audio/device.hpp`, `core/audio/include/pulp/audio/channel_set.hpp`, `core/audio/platform/mac/coreaudio_device.mm`, `core/audio/platform/win/wasapi_device.cpp`, `core/audio/platform/linux/alsa_device.cpp`, `core/audio/platform/linux/jack_device.cpp` |
| Native window embedding | Partially implemented | `core/view/include/pulp/view/window_host.hpp`, `core/view/include/pulp/view/plugin_view_host.hpp`, `core/view/src/window_host_stub.cpp`, `core/view/src/plugin_view_host_stub.cpp`, `core/host/include/pulp/host/plugin_slot.hpp` |
| OSC | In closure | `core/osc/include/pulp/osc/osc.hpp`, `core/osc/include/pulp/osc/bundle.hpp`, `core/osc/src/osc.cpp`, `core/osc/src/osc_udp.cpp`, `core/osc/src/bundle.cpp`, `core/osc/src/osc_channel.cpp`, `test/test_osc.cpp`, `test/test_osc_channel.cpp` |

## Gaps Worth Closing

### 1. Runtime and Process API Cleanup

Pulp has three child-process surfaces:
- `pulp::platform::ChildProcess`
- `pulp::runtime::{run_process, launch_process, is_process_running}`
- `pulp::events::ConnectedChildProcess`, which adds named-pipe IPC on top of
  the runtime process helpers

This creates overlap and makes it harder to know which API should own timeout,
output capture, cancellation, and process-lifecycle behavior.

Recommended work:
- Make one process API canonical. PR1 makes `pulp::platform::ChildProcess`
  canonical for handles, timeout, cancellation, process id, and stdout/stderr
  capture policy.
- Keep a compatibility wrapper only if needed. PR1 keeps
  `pulp::runtime::run_process` as a blocking compatibility wrapper and removes
  the PID-only runtime helpers.
- Add focused tests for timeout, cancellation, stdout/stderr capture, PATH lookup,
  and non-blocking wait. PR1 adds coverage for disabled stdout/stderr capture,
  process id reporting, runtime timeout propagation, connected child exit codes,
  child cancellation, manager cleanup, and named-pipe IPC framing.
- Decide whether `ConnectedChildProcess` stays on the runtime helpers or
  migrates onto the canonical process surface so timeout and cancellation
  semantics are shared. PR1 migrates it to own a `pulp::platform::ChildProcess`
  handle directly.
- Replace `runtime::launch_process`'s PID-only return with a real handle or
  remove it during consolidation; a bare PID gives callers no wait, cancel, or
  output-capture path. PR1 removes this helper instead of adding another
  handle type.
- Add a small current-process utility API only for demonstrated needs:
  debugger detection, process priority, foreground activation, and hard
  termination should be separate, platform-gated calls. PR1 does not add this
  surface because no in-tree caller justified it.

PR1 additional finding:
- `ConnectedChildProcess` depends on bidirectional `InterprocessConnection`
  over named pipes. The POSIX `NamedPipe` implementation used one read/write
  FIFO per public name, which allowed a connection read thread to consume its
  own outbound frame and made connected-child IPC flaky. PR1 changes POSIX
  `NamedPipe` to use directional paired FIFOs under the same public name, keeps
  `create_server()` blocking until a peer attaches, reports peer EOF for
  graceful and abrupt exits, protects macOS FIFO writes from `SIGPIPE`, and
  adds tests for named-pipe framed message exchange, peer close, FIFO cleanup,
  connected child exit, kill, relaunch, and manager cleanup.
- `ConnectedChildProcess` now captures real child exit status through the
  canonical platform process handle, tears down a pending IPC server if launch
  fails or the child never connects, and avoids self-join/concurrent-join races
  in monitor thread cleanup.
- `ChildProcessManager` now snapshots children before `kill_all()` and
  `wait_all()` so callbacks can safely call back into the manager. The manager
  callback keeps a temporary `shared_ptr` alive while handing callers the
  existing raw pointer.

PR1 local validation:
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF`
- `cmake --build build --target pulp-test-child-process pulp-test-runtime-utils
  pulp-test-stream pulp-test-ipc pulp-connected-child-process-fixture`
- `./build/test/pulp-test-child-process` — 186 assertions in 44 test cases
- `./build/test/pulp-test-runtime-utils` — 708 assertions in 152 test cases
- `./build/test/pulp-test-stream` — 410 assertions in 51 test cases
- `./build/test/pulp-test-ipc` — 170 assertions in 33 test cases
- Claude review pass found no remaining correctness blockers after fixes; low
  notes for oversize IPC frames and Windows std-handle fallback were fixed in
  this branch.
- RepoPrompt review pass found three P1 gaps after the first fix sweep:
  directional POSIX pipe connection semantics, manager callback pointer
  lifetime, and ignored IPC server startup failure. All three are fixed and
  covered by focused tests.
- Fresh RepoPrompt review then found a Windows pending `ConnectNamedPipe()`
  cancellation gap; PR1 now uses overlapped Windows pipe connects with
  `CancelIoEx()` so launch failure and connect-timeout paths cannot hang while
  joining the server thread.
- Follow-up Claude review found medium risks in disconnect-frame writes,
  client-side POSIX FIFO attach races, and unsynchronized platform child-process
  state. PR1 now relies on transport EOF instead of writing a disconnect frame
  during close, retries POSIX client `ENXIO` attach races, and serializes
  platform `ChildProcess` state with recursive mutexes.
- Final review notes for Windows overlapped pipe cleanup and
  `read_available_output()` locking were addressed; the final RepoPrompt pass
  and callback-drain follow-up review reported no remaining correctness
  blockers or high-confidence bugs.
- `tools/check-docs.sh` passed with existing repository warnings.
- Manual diff coverage was run with the same coverage pipeline and
  `PULP_ENABLE_GPU=OFF` because `tools/scripts/local_diff_cover.sh` re-enables
  GPU and fails locally without Skia. Result: 79% diff coverage against the
  75% floor.

PR1 submit and review sweep:
- PR: https://github.com/danielraffel/pulp/pull/2815
- Initial Shipyard submission opened the PR and ran local macOS validation.
  SSH-backed Windows/Ubuntu targets are not part of this focused validation
  pass; use GitHub-hosted platform checks and local/macOS evidence instead.
- Codex review P2 found that the runtime `run_process()` wrapper inherited the
  canonical platform child-process output cap. The compatibility wrapper now
  preserves prior unbounded stdout/stderr capture, covered by a >1 MiB
  stdout/stderr regression test.
- CI found that `test_stream.cpp` temp paths were deterministic per CTest
  process, allowing concurrently filtered FIFO tests to collide on the same
  path. Test helper paths now include process id and an atomic counter.
- macOS sanitizer CI exposed a POSIX reply-FIFO attach race where the client
  read thread could observe initial EOF before the server opened the reply
  writer, disconnecting before the first send. `NamedPipe::read()` now waits
  through that initial EOF window until data arrives or the peer is confirmed
  closed; a focused FIFO regression test covers the race. Local ASan and UBSan
  builds pass the original failing IPC exchange and the new regression test.
- Final Claude review of the reply-FIFO attach fix reported no blocking
  correctness bugs; the only follow-up was naming the bounded initial EOF
  window for auditability.
- macOS ThreadSanitizer CI then exposed POSIX named-pipe teardown races on file
  descriptor ownership and server unlink state while manager cleanup was
  joining active IPC children. The POSIX descriptor slots and server flag are
  now atomic, setup publishes descriptors only after both directions are fully
  configured, and local TSan IPC/manager coverage passes the affected cases.
- A follow-up TSan sweep of the new wait/manager coverage exposed a
  `ConnectedChildProcess` monitor-thread publication race: a fast-exiting child
  could invoke `on_exit`, call `wait_for_exit()`, and inspect `monitor_thread_`
  while `launch()` was still assigning the thread object. RepoPrompt review
  also identified the same launch-time window for message callbacks. PR1 now
  publishes and awaits the monitor thread under the same mutex used by
  join/detach, uses atomic PID publication for callback-time reads, notifies
  exit waiters before IPC read-thread disconnect joins, joins the monitor
  outside the monitor mutex to avoid reentrant callback deadlocks, documents
  and enforces that the object must outlive message callbacks by stopping new
  callback entry during destruction and waiting for active callbacks to drain.
  The focused coverage now includes exit callback wait, message callback
  kill/wait plus callback-time PID reads, concurrent `wait_for_exit()` callers,
  relaunch after callback-driven kill, and active-callback destruction under
  normal, ASan, UBSan, and TSan runs.
- macOS ThreadSanitizer CI also exposed a `HighResolutionTimer` self-stop race:
  a fast callback could call `stop()` while `start()` was still assigning the
  worker `std::thread` member. PR1 now serializes timer thread-handle access,
  blocks callbacks until the handle is published, and keeps self-stop and
  callback-owned destruction TSan-clean.
- macOS AddressSanitizer CI exposed an unrelated parallel-test collision in
  `test_mcp_server.cpp` temp homes. The test helper now includes process id and
  an atomic sequence in temp paths; the failing MCP status case passes locally
  under ASan, including with adjacent MCP tests running in parallel.
- macOS UndefinedBehaviorSanitizer CI also surfaced the known CHOC/QuickJS
  runtime-shim failure tracked as #1987 in additional design-import fixtures.
  Rather than skip more assertions, PR1 now compiles only the QuickJS backend
  translation unit without UBSan instrumentation while keeping Pulp's script and
  view code instrumented. The previously failing Figma, Stitch, Pencil, and
  baked-native materialization cases now pass locally under UBSan.
- PR #2815 was squash-merged into `main` on 2026-05-25.
- GitHub-hosted Linux CI surfaced a latent OSC UDP portability gap after PR1:
  `Receiver` enabled `SO_REUSEADDR`, which lets Linux bind multiple receivers
  to the same UDP port and breaks the existing exclusive-listener contract. That
  fix is carried in PR2 because PR1 had already merged.

### 2. Main-Thread Dispatch and Native Event Loop

`EventLoop` is useful, but it always owns its own worker thread. That is not the
same thing as marshalling work onto the platform UI thread or integrating with a
native application message pump.

Recommended work:
- Introduce a `MainThreadDispatcher` or equivalent UI-thread dispatch contract.
- Provide `call_async`, `call_sync`, and `is_main_thread` semantics.
- Wire platform window hosts to register their native UI thread dispatcher.
- Keep `EventLoop` as a worker-loop primitive instead of overloading it with UI
  message-loop responsibility.
- Fix `EventLoop::thread_id_` initialization while working in this area. It is
  assigned inside the worker thread today, so a concurrent `is_current_thread()`
  call during construction can observe the default id.
- Add tests that prove worker-thread calls can safely marshal to the UI thread.

Native event loop local implementation status:
- Added `pulp::events::MainThreadDispatcher` as the process-wide native
  main-thread dispatch surface. The dispatcher supports `call_async`,
  `call_sync`, `is_main_thread`, backend presence checks, token-based
  registration, and stacked backend restoration when nested owners unregister.
- Hardened backend lifetime semantics. Dispatcher calls lease the selected
  backend while invoking `post` or `is_main_thread`; `unregister_backend()`
  removes the token immediately, waits for other in-flight callbacks, and
  handles self-unregister from backend callbacks without deadlocking.
- Made `call_sync()` revalidate backend liveness before inline execution or
  posting. If a backend unregisters during `is_main_thread()`, the dispatcher
  reacquires the restored active backend instead of using a stale callback set.
- Wrapped `call_async()` tasks before handing them to native backends so user
  exceptions cannot escape into AppKit/UIKit/SDL queues. `call_sync()` still
  propagates task exceptions to the caller and now has a bounded backend-retry
  loop if registrations churn during dispatch.
- Kept `EventLoop` as a worker-loop primitive while fixing lifecycle hazards:
  thread id publication and reads are synchronized under the loop mutex,
  mutable loop state is held by shared state so self-thread destruction can
  complete, post-stop enqueues are rejected under the loop mutex, and a
  self-stop prevents later tasks in the drained batch from running.
- Registered native backends in platform hosts:
  - macOS Cocoa hosts dispatch through `dispatch_get_main_queue()` while their
    application loop is running, reject new work once app shutdown begins, and
    defer `[NSApp stop:nil]` so already accepted main-queue work can drain.
  - iOS UIKit hosts dispatch through `dispatch_get_main_queue()` and retain
    registration tokens across the non-blocking iOS run-loop handoff.
  - SDL hosts register a loop-thread queue that drains inside the SDL event
    loop, rejects new work during shutdown, and drains one task snapshot per
    loop tick so self-reposting tasks cannot starve SDL polling.
- While wiring the platform hosts, fixed adjacent teardown hazards found during
  review: macOS CPU idle timers are invalidated on host destruction, queued
  macOS GPU render blocks carry a liveness token, macOS/iOS GPU idle and resize
  callbacks re-check liveness after user callbacks, and iOS CPU/GPU window
  teardown disconnects retained UIKit callbacks before host-owned state is
  cleared.
- Hosted TSan then exposed an IPC timing assumption outside the dispatcher
  surface: `ChildProcessManager wait_all waits for active connected children`
  relied on 75/150 ms child-process sleeps, which is not stable once process
  launch is sanitizer-instrumented. The connected-child fixture now supports an
  explicit parent-driven `exit` message so the test proves cleanup/wait behavior
  without wall-clock races.
- The same local TSan sweep found two real IPC socket races that would have
  become the next sanitizer failures: server stop closed the listening socket
  before the accept thread joined, and connection disconnect closed a socket
  while the read thread was still blocked in `receive()`. Server stop now wakes
  accept before joining and closes after join; connection disconnect now
  interrupts blocking socket reads with `Socket::shutdown()`, joins the read
  thread, then closes the handle.
- Accepted socket connections can publish lambda callbacks through synchronized
  setter methods before active read-thread callbacks observe them. Direct public
  callback field assignment remains valid before a connection starts; connected
  instances should use the setters.

Native event loop local validation:
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF`
- `cmake --build build --target pulp-test-events pulp-view-core
  -j$(sysctl -n hw.ncpu)`
- `./build/test/pulp-test-events
  "[events][main_thread_dispatcher],[events][event_loop]"` passed: 168
  assertions in 34 focused test cases.
- `./build/test/pulp-test-events --durations yes` passed: 259 assertions in
  57 test cases.
- `./build/test/pulp-test-ipc` passed: 173 assertions in 33 test cases.
- TSan validation:
  `cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug
  -DCMAKE_CXX_FLAGS="-fsanitize=thread" -DCMAKE_C_FLAGS="-fsanitize=thread"
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" -DPULP_BUILD_TESTS=ON
  -DPULP_BUILD_EXAMPLES=OFF -DPULP_ENABLE_GPU=OFF`,
  `cmake --build build-tsan --target pulp-test-ipc pulp-test-events -j8`,
  `TSAN_OPTIONS="halt_on_error=1:suppressions=$PWD/test/tsan.supp"
  ./build-tsan/test/pulp-test-ipc` passed: 173 assertions in 33 test cases,
  `TSAN_OPTIONS="halt_on_error=1:suppressions=$PWD/test/tsan.supp"
  ./build-tsan/test/pulp-test-events` passed: 259 assertions in 57 test cases,
  and the focused dispatcher/event-loop CTest subset passed 34/34.
- Manual GPU-off coverage build:
  `cmake -S . -B build-cov-dispatch -DCMAKE_BUILD_TYPE=Debug
  -DPULP_ENABLE_COVERAGE=ON -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`,
  `cmake --build build-cov-dispatch --target pulp-test-events
  pulp-test-ipc -j8`, then
  `LLVM_PROFILE_FILE="$PWD/build-cov-dispatch/profraw/pulp-%p-%m.profraw"
  ctest --test-dir build-cov-dispatch --output-on-failure
  -R 'IPC|ChildProcess|ConnectedChildProcess|MainThread|EventLoop|event
  loop|dispatcher|Dispatcher'` passed 68/68 tests.
- Manual diff coverage passed against `origin/main`: 93% diff coverage. The
  per-tier gate also passes after wiring it to the same shared diff-cover
  exclusion set: audio-critical no touched lines, user-facing no counted touched
  lines, infrastructure 53.8% against the 50% floor. llvm-cov reported the two
  `EventLoop` `condition_variable::notify_one()` lines and several defensive
  dispatcher branches as uncovered; the exercised dispatcher behavior is covered
  by focused registration, unregister, sync, async, exception, and
  backend-restore tests. `window_host_mac.mm`, `window_host_ios.mm`, and
  `sdl_window_host.cpp` are excluded in the shared coverage config as live
  native window-host loops; the dispatcher contract they use is covered by the
  focused unit tests.
- Coverage-tooling regressions are covered by:
  `python3 tools/scripts/test_coverage_tier_check.py` — 46 tests,
  `python3 tools/scripts/test_codecov_config.py` — 17 tests, and
  `python3 tools/scripts/test_local_diff_cover.py` — 17 tests.
- `git diff --check` passed.
- After the branch was rebased onto `origin/main` at `0939e9b19`, the stale
  `0.209.0` version bump remained dropped. After `main` advanced through
  `0.235.0`, this branch now carries a fresh required SDK bump to `0.236.0`.
  Focused Release/GPU-off validation passed:
  `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF`,
  `cmake --build build --target pulp-test-events pulp-test-ipc pulp-test-osc
  pulp-test-osc-channel -j8`, and
  `ctest --test-dir build --output-on-failure -R
  "EventLoop|MainThread|main-thread|dispatcher|IPC|Interprocess|OscChannel|OSC
  Receiver rejects binding"` 67/67. This explicitly re-ran the two OSC
  bind-collision cases that failed on the stale hosted Linux merge.
- A fresh PR review sweep on the rebased head found one P1 in
  `InterprocessConnectionServer::stop()`: the accept thread was joined before
  the listening socket closed, leaving shutdown dependent on a best-effort
  self-connect wake. The fix closes the listener before join, and focused
  regression coverage verifies that a stopped socket server releases its port
  for immediate reuse. Validation after the fix:
  `cmake --build build --target pulp-test-events pulp-test-ipc -j8` and
  `ctest --test-dir build --output-on-failure -R
  'EventLoop|MainThread|main-thread|dispatcher|IPC|Interprocess'` passed
  54/54.
- Hosted GitHub checks on the open platform PR stack then exposed shared
  portability issues outside the dispatcher implementation: the design-debug
  helper used POSIX `popen`/`pclose` names that MSVC does not provide, the
  inspector stale-selection regression could be defeated by allocator address
  reuse on Linux, and the text-edit paste test used the macOS command modifier
  instead of the platform-primary modifier. The dispatcher branch carries these
  small shared fixes so #2822 and the native-window lane can rebase onto one
  green base. Validation after the fix: `git diff --check`;
  `cmake --build build --target pulp-test-events pulp-test-ipc -j8`;
  `ctest --test-dir build --output-on-failure -R
  'EventLoop|MainThread|main-thread|dispatcher|IPC|Interprocess'` passed
  54/54; `cmake -S . -B build-inspector-focus -DCMAKE_BUILD_TYPE=Release
  -DPULP_ENABLE_GPU=ON -DPULP_BUILD_EXAMPLES=OFF -DPULP_BUILD_TESTS=ON`;
  `cmake --build build-inspector-focus --target pulp-test-inspector
  pulp-test-design-debug-contracts -j8`; direct focused inspector cases for
  stale selection and platform-primary paste passed; and
  `ctest --test-dir build-inspector-focus --output-on-failure -R
  design-debug` passed 5/5.
- The next hosted Linux sweep exposed three more shared Linux assumptions:
  OSC receiver sockets enabled UDP address reuse, allowing a second listener to
  bind the same port on Linux; and the inspector paste test required a native
  clipboard even on hosted Linux images with no clipboard tool. The OSC
  receiver now keeps listener binds exclusive, and the inspector test follows
  the existing clipboard contract by skipping the paste half only when the
  platform reports an honest unsupported clipboard. Local validation after the
  fix: `cmake --build build --target pulp-test-osc pulp-test-osc-channel -j8`;
  direct focused OSC receiver and OscChannel occupied-port cases passed;
  `cmake --build build-inspector-focus --target pulp-test-inspector -j8`;
  direct focused platform-primary paste case passed; `ctest --test-dir build
  --output-on-failure -R
  'OSC|OscChannel|EventLoop|MainThread|main-thread|dispatcher|IPC|Interprocess'`
  passed 122/122; `ctest --test-dir build-inspector-focus --output-on-failure
  -R design-debug` passed 5/5; and `git diff --check` passed.
- Claude and RepoPrompt blocker reviews were run. Claude's P1 findings around
  async exceptions, EventLoop thread-id synchronization, iOS registration
  order, unbounded sync retry, and SDL drain starvation were fixed; the final
  RepoPrompt review reported no remaining P0/P1 findings after the macOS
  shutdown liveness fix.
- SSH-backed Windows/Ubuntu targets are intentionally out of scope for this
  focused validation pass; use GitHub-hosted platform checks and local/macOS
  evidence instead.

### 3. Audio Format Completion

The format registry supports WAV read/write, FLAC read, MP3 read, Ogg Vorbis
read, AIFF read/write, optional FLAC write, optional MP3 write, and Apple-only
CoreAudio decode support for additional formats. Missing or incomplete areas:
- no Windows Media reader/writer
- no Ogg Vorbis writer
- `.aifc` is listed in known extensions, but the AIFF reader advertises only
  `.aiff` and `.aif`
- AIFC compression FourCC is skipped, so compressed payloads can be interpreted
  as raw PCM instead of being rejected
- optional writers are compile-time gated, so capability reporting needs to be
  explicit

Recommended work:
- Add public read/write capability queries per extension and format name.
- Reject AIFC files whose `COMM` compression FourCC is not in `{NONE, sowt,
  fl32, fl64}`; only register `.aifc` once that whitelist is honored.
- Add an Ogg Vorbis writer if product workflows need Ogg export.
- Add Windows Media support behind a Windows-only backend if that remains a
  supported claim.
- Expose license tier per writer in capability queries so applications can
  distinguish bundled readers/writers from optional encoders with additional
  redistribution constraints.
- Add format matrix tests that assert read/write availability in the current
  build configuration.

### 4. Audio Device Manager Layer

Pulp has platform audio systems and devices, but no high-level device manager
that owns setup persistence, callback fanout, audio/MIDI routing, restart policy,
level metering, xrun reporting, or UI-facing selector state.

Blocking issue:
- `core/audio/platform/linux/jack_device.cpp` writes
  `DeviceInfo::default_sample_rate` and `DeviceInfo::is_default`, but the public
  `DeviceInfo` defines `sample_rates`, `is_default_input`, and
  `is_default_output`. The Linux+JACK build path is broken whenever the optional
  JACK backend is compiled. Fix this before broader device-manager work and add
  a JACK-enabled build lane or smoke test.

Recommended work:
- Add `AudioDeviceManager` above `AudioSystem`.
- Define a serializable setup object with input device, output device, sample
  rate, buffer size, active input channels, and active output channels.
- Let the manager own callback registration and output summing.
- Surface CPU load, level meters, xrun counts, and device errors.
- Propagate `ChannelSet` or speaker-layout metadata through `DeviceInfo`.
- Remove JACK's hard 8-channel processing cap or make it an explicit limit.
- Define how the manager chooses between ALSA, JACK, and JACK-compatible
  PipeWire paths when more than one backend is available.
- Add hotplug behavior tests for each backend that can expose real notifications.

### 5. Native Child Window Embedding

The embedding API is present in `WindowHost` and `PluginViewHost`, but built-in
concrete support is Apple-first. Non-Apple paths rely on host-registered
factories, and default child-view attachment returns `false`.

Recommended work:
- Ship first-party Windows and Linux `WindowHost` implementations.
- Ship first-party Windows and Linux `PluginViewHost` implementations.
- Implement child-view attach, resize, and detach for platform-native handles.
- Standardize the plugin editor lifecycle around `HostedEditor`.
- Add smoke tests for native child attach/detach and bounds updates on each
  supported desktop platform.

### 6. OSC Bundle and Routing Support

OSC messages over UDP are implemented. Bundles can be serialized and
deserialized, but sender/receiver APIs remain message-centric.

Recommended work:
- Add a typed `Sender::send(const Bundle&)` overload on top of the existing
  `send_raw(const uint8_t*, size_t)` primitive.
- Make `Receiver` detect `#bundle` packets and dispatch bundle callbacks.
- Wire the existing `address_matches()` helper into listener registration with
  address-pattern filtering.
- Preserve bundles through `OscChannel` or explicitly expose raw packet mode.
- Add format-error callbacks for invalid packets.
- Invoke the format-error callback when `Receiver` currently drops malformed
  packets via an empty-address decode result.
- Extend tests to cover nested bundles, bundle receive paths, address filtering,
  and invalid-packet handling.

PR2 implementation scope:
- `Sender::send(const Bundle&)` serializes typed bundles through the existing UDP
  datagram primitive and preserves the existing connected-state rejection path.
  Reconnect behavior is covered for bundle sends after `disconnect()` and a new
  target `connect()`.
- `Receiver::listen_with_options()` adds a non-ambiguous options-based API while
  preserving the existing `listen(port, handler)` source contract, including
  `listen(port, {})`.
- `ReceiverOptions` exposes top-level `on_bundle`, `on_message`, `on_error`, and
  address-pattern `ReceiverRoute` callbacks. Direct messages and messages inside
  nested bundles are routed through the existing `address_matches()` matcher.
- The receiver detects `#bundle` datagrams, deserializes typed bundles, reports
  malformed bundles through `on_error`, and reports malformed message datagrams
  through `on_error` instead of silently dropping them when rich receiver options
  are used, including valid zero-length UDP datagrams that are malformed OSC
  packets. Empty bundles dispatch through `on_bundle` without synthetic message
  callbacks.
- The UDP receive path now sizes its packet buffer for full UDP datagrams, so
  valid bundles larger than the old 4 KiB stack buffer are delivered instead of
  being parsed as truncated malformed packets.
- UDP receivers now bind exclusively so the one-receiver-per-port contract is
  consistent on Linux and macOS; an explicit shared-port mode can be added later
  if a real API need appears. The Windows path sets `SO_EXCLUSIVEADDRUSE`
  before `bind()` to preserve the same contract against sockets that opt into
  shared address use.
- `Receiver::listen_with_options()` now treats receive-timeout setup failure as
  a listen failure instead of starting a receiver that may not stop promptly.
- Receiver callbacks can request `stop()` from the receiver thread without
  self-joining; cleanup is completed by the next outside `stop()` or
  destructor call, the receiver can be reused without requiring a manual cleanup
  call first, and callback fanout for the current datagram stops immediately
  once shutdown has been requested. Receiver worker state is ref-counted so
  destroying a receiver-owned object from inside a callback cannot free the
  socket/thread state before the receive thread unwinds.
- `Sender::connect()` now commits a new UDP socket and destination only after
  host resolution succeeds, so a failed reconnect cannot leak the previous
  socket or leave the sender disconnected from its existing destination.
- `Bundle::deserialize()` now rejects null data even when the caller provides a
  bundle-sized length, matching the rest of the malformed-packet rejection
  behavior.
- Address-pattern validation now fails closed for trailing empty alternatives
  such as `{foo,}` and `{foo,bar,}`.

PR2 validation and PR state:
- PR #2822: https://github.com/danielraffel/pulp/pull/2822
- The branch is rebased onto `origin/main` at `ad72d830` after #2825, #2905,
  #2900, #2909, #2906, #2914, #2908, #2898, #2913, the regenerated v0.238.0
  changelog, #2901, and #2902 landed. The fresh required SDK bump is
  `0.242.0`.
- Initial PR review-comment and issue-comment sweep found no comments to
  address. The first CI failure was the version-bump commit-shape guard; it was
  fixed by splitting the `0.209.0` API bump into the canonical
  `chore: bump versions` commit. The refreshed version/skill check is passing.
- A later PR sweep found no human review threads. The stale pre-rebase hosted
  matrix had unrelated Linux inspector failures and Windows process/IPC/
  inspector/import-design failures; no SSH Windows/Ubuntu validation is part of
  this focused pass. The stale Codecov bot report had patch coverage below the
  advisory 75% target, so this branch was rebased onto current `main` and the
  OSC coverage tests were tightened before pushing a refreshed head.
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF`
- `cmake --build build --target pulp-test-osc pulp-test-osc-bundle
  pulp-test-osc-channel`
- Focused CTest coverage for route matching, typed bundle send/receive,
  malformed message/bundle callbacks, exclusive receiver binding, receiver
  restart rejection, occupied local-port channel failure, successful sender
  reconnect over an existing socket, empty route handlers, and receiver-thread
  listen rejection after callback-requested stop passed.
- `ctest --test-dir build --output-on-failure -j4 --timeout 120 -R
  'OSC|OscChannel|osc|Bundle::deserialize rejects null data with bundle-sized
  length'` passed 100/100 after the final unconnected bundle-send,
  failed-reconnect, large-bundle receive, callback self-stop, and null
  bundle-deserialize assertions were added. The callback self-stop coverage also
  verifies that a receiver can restart after the callback-requested stop and
  that stop short-circuits bundle, message, route, and nested bundle callback
  fanout for the current datagram. Callback-time receiver-owner destruction is
  covered as a lifecycle regression. Malformed-input coverage includes real
  zero-length UDP datagrams and trailing-empty address-pattern alternatives.
  Bundle transport coverage includes disconnect/reconnect sends and empty bundle
  receive. A fresh focused release run after the rebase passed
  `ctest --test-dir build --output-on-failure -R 'OSC|OscChannel'` 100/100.
- After the `0939e9b19` rebase, focused Release/GPU-off validation passed:
  `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF`,
  `cmake --build build --target pulp-test-osc pulp-test-osc-channel -j8`,
  and `ctest --test-dir build --output-on-failure -R 'OSC|OscChannel'`
  100/100.
- A fresh PR review sweep on the rebased head found one P1 around callback-
  thread `Receiver::stop()` leaving the UDP socket bound until a later owner
  stop/destructor. The fix now closes the UDP socket immediately on the
  receiver thread while still deferring thread join to an outside stop. A
  Claude follow-up review found a stale-FD race from adding a receiver-thread
  socket writer; the socket handle now uses an atomic claim/exchange handoff so
  exactly one stop path owns shutdown/close. Focused coverage now rebinds
  another receiver to the same port after callback-thread `stop()` returns and
  before the original owner joins the receive thread. Validation after the fix:
  `cmake --build build --target pulp-test-osc pulp-test-osc-channel -j8` and
  `ctest --test-dir build --output-on-failure -R 'OSC|OscChannel'` passed
  100/100.
- After the `6125c235a` rebase, local post-rebase validation passed:
  `cmake --build build --target pulp-test-osc pulp-test-osc-channel
  pulp-test-events pulp-test-ipc -j8`,
  `ctest --test-dir build --output-on-failure -R 'OSC|OscChannel'` 100/100,
  and
  `ctest --test-dir build --output-on-failure -R
  'OSC|OscChannel|EventLoop|MainThread|main-thread|dispatcher|IPC|Interprocess'`
  154/154. `python3 tools/scripts/version_bump_check.py --mode=report
  --accept-intent-trailers` passes for SDK `0.240.0`, and `git diff --check`
  passes.
- After the `2d8f004a` rebase, local post-rebase validation passed:
  `cmake --build build --target pulp-test-osc pulp-test-osc-channel
  pulp-test-events pulp-test-ipc -j8` and
  `ctest --test-dir build --output-on-failure -R
  'OSC|OscChannel|EventLoop|MainThread|main-thread|dispatcher|IPC|Interprocess'`
  154/154. `python3 tools/scripts/version_bump_check.py --base origin/main
  --config tools/scripts/versioning.json --mode=report
  --require-bump-for-fix-feat --accept-intent-trailers` passes for SDK
  `0.241.0`, and `git diff --check` passes.
- After the `0d934d4c` rebase, local post-rebase validation passed:
  `cmake --build build --target pulp-test-osc pulp-test-osc-channel
  pulp-test-events pulp-test-ipc -j8` and
  `ctest --test-dir build --output-on-failure -R
  'OSC|OscChannel|EventLoop|MainThread|main-thread|dispatcher|IPC|Interprocess'`
  154/154. `python3 tools/scripts/version_bump_check.py --base origin/main
  --config tools/scripts/versioning.json --mode=report
  --require-bump-for-fix-feat --accept-intent-trailers` passes for SDK
  `0.241.0`, and `git diff --check` passes.
- After the `4cd4bdb` rebase, local post-rebase validation passed:
  `cmake --build build --target pulp-test-osc pulp-test-osc-channel
  pulp-test-events pulp-test-ipc -j8` and
  `ctest --test-dir build --output-on-failure -R
  'OSC|OscChannel|EventLoop|MainThread|main-thread|dispatcher|IPC|Interprocess'`
  154/154. `python3 tools/scripts/version_bump_check.py --base origin/main
  --config tools/scripts/versioning.json --mode=report
  --require-bump-for-fix-feat --accept-intent-trailers` passes for SDK
  `0.241.0`, `tools/check-docs.sh` passes with 76 existing warnings, and
  `git diff --check` passes.
- After the `90a21146f` rebase, local post-rebase validation passed:
  `cmake --build build --target pulp-test-osc pulp-test-osc-channel
  pulp-test-events pulp-test-ipc -j8` and
  `ctest --test-dir build --output-on-failure -R
  'OSC|OscChannel|EventLoop|MainThread|main-thread|dispatcher|IPC|Interprocess'`
  154/154. `python3 tools/scripts/version_bump_check.py --base origin/main
  --config tools/scripts/versioning.json --mode=report
  --require-bump-for-fix-feat --accept-intent-trailers` passes for SDK
  `0.242.0`, `tools/check-docs.sh` passes with 76 existing warnings, and
  `git diff --check` passes.
- After the `ad72d830` rebase, local post-rebase validation passed:
  `cmake --build build --target pulp-test-osc pulp-test-osc-channel
  pulp-test-events pulp-test-ipc -j8` and
  `ctest --test-dir build --output-on-failure -R
  'OSC|OscChannel|EventLoop|MainThread|main-thread|dispatcher|IPC|Interprocess'`
  154/154. `python3 tools/scripts/version_bump_check.py --base origin/main
  --config tools/scripts/versioning.json --mode=report
  --require-bump-for-fix-feat --accept-intent-trailers` passes for SDK
  `0.242.0`, `tools/check-docs.sh` passes with 76 existing warnings, and
  `git diff --check` passes.
- After `main` advanced to `24f1eba` through #2929 and consumed SDK
  `0.243.0`, the stale `0.242.0` bump commit was skipped during rebase and a
  fresh required SDK bump to `0.244.0` was applied. Local post-rebase
  validation passed:
  `cmake --build build --target pulp-test-osc pulp-test-osc-channel
  pulp-test-events pulp-test-ipc -j8` and
  `ctest --test-dir build --output-on-failure -R
  'OSC|OscChannel|EventLoop|MainThread|main-thread|dispatcher|IPC|Interprocess'`
  154/154. `python3 tools/scripts/version_bump_check.py --base origin/main
  --config tools/scripts/versioning.json --mode=report
  --require-bump-for-fix-feat --accept-intent-trailers` passes for SDK
  `0.244.0`, `tools/check-docs.sh` passes with 76 existing warnings, and
  `git diff --check` passes.
- After `main` advanced again to `95d8532` through #2931 and consumed SDK
  `0.244.0`, Git dropped the stale `0.244.0` bump as already upstream during
  rebase and a fresh required SDK bump to `0.245.0` was applied. Local
  post-rebase validation passed:
  `cmake --build build --target pulp-test-osc pulp-test-osc-channel
  pulp-test-events pulp-test-ipc -j8` and
  `ctest --test-dir build --output-on-failure -R
  'OSC|OscChannel|EventLoop|MainThread|main-thread|dispatcher|IPC|Interprocess'`
  154/154. `python3 tools/scripts/version_bump_check.py --base origin/main
  --config tools/scripts/versioning.json --mode=report
  --require-bump-for-fix-feat --accept-intent-trailers` passes for SDK
  `0.245.0`, `tools/check-docs.sh` passes with 76 existing warnings, and
  `git diff --check` passes.
- Hosted CI then surfaced a repo-audit false positive after `main` introduced
  the first-party host-quirk header `core/format/include/pulp/format/host_quirks/pro_tools.hpp`.
  The audit scanner still correctly blocks vendor-shaped files elsewhere, but
  now allowlists that Pulp-owned header. The branch was rebased again onto
  `origin/main` at `acf025a` after #2932, clearing the skill-sync range race.
  Local validation passed: `python3 tools/scripts/test_audit_top_level.py` 10/10,
  `python3 tools/audit.py --`, `cmake --build build --target pulp-test-osc
  pulp-test-osc-channel pulp-test-events pulp-test-ipc -j8`, `ctest --test-dir
  build -R '^(OscChannel|OSC|IPC|EventLoop)' --output-on-failure` 132/132,
  version-bump report for SDK `0.245.0`, `tools/check-docs.sh` with 76 existing
  warnings, and `git diff --check`.
- `main` advanced again to `ebf75ab` through #2934 and consumed SDK `0.245.0`
  while #2822 hosted checks were starting. The stale bump was dropped during
  rebase and a fresh required SDK bump to `0.246.0` was applied. Local
  validation passed again: `python3 tools/scripts/test_audit_top_level.py`
  10/10, `python3 tools/audit.py --`, `cmake --build build --target
  pulp-test-osc pulp-test-osc-channel pulp-test-events pulp-test-ipc -j8`,
  `ctest --test-dir build -R '^(OscChannel|OSC|IPC|EventLoop)'
  --output-on-failure` 132/132, version-bump report for SDK `0.246.0`,
  `tools/check-docs.sh` with 76 existing warnings, and `git diff --check`.
- Hosted CI on the `7e5c401a3` head then failed `Enforce version & skill sync`
  because `main` advanced through #2935, #2936, #2937, and the v0.245.0
  changelog while the checks were running. The resulting PR comparison included
  unrelated main-side audio/format adapter paths, so the skill-sync failure was
  a stale-base range race rather than an OSC implementation failure. The branch
  was rebased onto `origin/main` at `1a11c516`; the stale `0.246.0` bump was
  skipped as already upstream, and a fresh required SDK bump to `0.247.0` was
  applied. Local validation passed again: `python3
  tools/scripts/test_audit_top_level.py` 10/10, `python3 tools/audit.py --`,
  `python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD
  --mode report`, `python3 tools/scripts/version_bump_check.py --base
  origin/main --config tools/scripts/versioning.json --mode=report
  --require-bump-for-fix-feat --accept-intent-trailers` for SDK `0.247.0`,
  `cmake --build build --target pulp-test-osc pulp-test-osc-channel
  pulp-test-events pulp-test-ipc -j8`, `ctest --test-dir build -R
  '^(OscChannel|OSC|IPC|EventLoop)' --output-on-failure` 132/132, and
  `tools/check-docs.sh` with 76 existing warnings.
- `main` advanced again to `7be39f4a` while hosted checks for `0c99e2cd2`
  were queued, consuming SDK `0.247.0` in the item 3.4/3.5/3.10 bundle. The
  stale `0.247.0` bump was dropped during rebase, and a fresh required SDK bump
  to `0.248.0` was applied. Local validation passed again: `python3
  tools/scripts/test_audit_top_level.py` 10/10, `python3 tools/audit.py --`,
  `python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD
  --mode report`, `python3 tools/scripts/version_bump_check.py --base
  origin/main --config tools/scripts/versioning.json --mode=report
  --require-bump-for-fix-feat --accept-intent-trailers` for SDK `0.248.0`,
  `cmake --build build --target pulp-test-osc pulp-test-osc-channel
  pulp-test-events pulp-test-ipc -j8`, `ctest --test-dir build -R
  '^(OscChannel|OSC|IPC|EventLoop)' --output-on-failure` 132/132,
  `tools/check-docs.sh` with 76 existing warnings, and `git diff --check`.
- Hosted install-consumer smoke then exposed an installed-SDK packaging gap:
  `PulpConfig.cmake` loaded `PulpTargets.cmake` before defining the private
  static dependency target `SheenBidi::SheenBidi`, so downstream
  `find_package(Pulp)` consumers failed while validating exported targets. The
  branch now loads SheenBidi through `find_dependency(SheenBidi CONFIG)` before
  `PulpTargets.cmake`. Local validation rebuilt and installed a clean
  Release/GPU-off SDK to `/tmp/pulp-install-check`, verified the installed
  SheenBidi package files are present, and configured a downstream
  `find_package(Pulp)` + `pulp_add_plugin(... FORMATS CLAP ...)` consumer that
  explicitly checks `TARGET SheenBidi::SheenBidi`.
- The refreshed hosted Ubuntu install-consumer smoke then passed on the
  SheenBidi fix, covering the downstream `find_package(Pulp)` + install-prefix
  path in CI. The same hosted check wave hit GitHub Actions infrastructure
  failures unrelated to branch code: multiple jobs failed before checkout with
  `remote: Your account is suspended`/HTTP 403, coverage failed while
  downloading actions from `codeload.github.com`, and rescue/rerun left several
  workflows queued without jobs or cancelled before steps ran. An empty
  `chore: retrigger OSC CI` commit was pushed at `8a8ddd3ad`, followed by this
  living-doc update, to force a clean PR check set without changing OSC code.
- GitHub resolved the Actions/Pages incident at 2026-05-26 13:18 UTC and
  Actions returned to operational status. The branch was then rebased cleanly
  onto `origin/main` at `bae205c3` after `main` advanced through the coverage
  PR stack without consuming SDK `0.248.0`. Post-rebase local validation
  passed: `python3 tools/scripts/test_audit_top_level.py` 10/10,
  `python3 tools/audit.py --`, `python3 tools/scripts/skill_sync_check.py
  --base origin/main --head HEAD --mode report`,
  `python3 tools/scripts/version_bump_check.py --base origin/main --config
  tools/scripts/versioning.json --mode=report --require-bump-for-fix-feat
  --accept-intent-trailers` for SDK `0.248.0`, Release/GPU-off configure and
  build of `pulp-test-osc`, `pulp-test-osc-channel`, `pulp-test-events`, and
  `pulp-test-ipc`, `ctest --test-dir build -R '^(OscChannel|OSC|IPC|EventLoop)'
  --output-on-failure` 132/132, Release flag verification for
  `pulp-test-osc`, `tools/check-docs.sh` with the existing 76 warnings, and
  diff hygiene.
- While the refreshed `c77a22d79` checks were running, `main` advanced through
  #2958/#2959, regenerated the v0.248.0 changelog, and consumed SDK
  `0.248.0`. The branch was rebased again onto `origin/main` at `e7cc41b0`;
  the now-upstream `0.248.0` bump was dropped during rebase, and a fresh SDK
  bump to `0.249.0` was applied. Local validation passed again: `python3
  tools/scripts/test_audit_top_level.py` 10/10, `python3 tools/audit.py --`,
  `python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD
  --mode report`, `python3 tools/scripts/version_bump_check.py --base
  origin/main --config tools/scripts/versioning.json --mode=report
  --require-bump-for-fix-feat --accept-intent-trailers` for SDK `0.249.0`,
  Release/GPU-off configure and build of `pulp-test-osc`,
  `pulp-test-osc-channel`, `pulp-test-events`, and `pulp-test-ipc`,
  `ctest --test-dir build -R '^(OscChannel|OSC|IPC|EventLoop)'
  --output-on-failure` 132/132, Release flag verification for
  `pulp-test-osc`, `tools/check-docs.sh` with the existing 76 warnings, and
  diff hygiene.
- While the refreshed `0bae9c202` checks were running, `main` advanced through
  #2963, regenerated the v0.249.0 changelog, and consumed SDK `0.249.0`. The
  branch was rebased again onto `origin/main` at `e68b1cbd`; the now-upstream
  `0.249.0` bump was dropped during rebase, and a fresh SDK bump to `0.250.0`
  was applied. Local validation passed again: `python3
  tools/scripts/test_audit_top_level.py` 10/10, `python3 tools/audit.py --`,
  `python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD
  --mode report`, `python3 tools/scripts/version_bump_check.py --base
  origin/main --config tools/scripts/versioning.json --mode=report
  --require-bump-for-fix-feat --accept-intent-trailers` for SDK `0.250.0`,
  Release/GPU-off configure and build of `pulp-test-osc`,
  `pulp-test-osc-channel`, `pulp-test-events`, and `pulp-test-ipc`,
  `ctest --test-dir build -R '^(OscChannel|OSC|IPC|EventLoop)'
  --output-on-failure` 132/132, Release flag verification for
  `pulp-test-osc`, `tools/check-docs.sh` with the existing 76 warnings, and
  diff hygiene.
- While the refreshed `27bacda5c` checks were running, `main` advanced through
  #2966/#2967 and consumed SDK `0.250.0`. The branch was rebased again onto
  `origin/main` at `a7e223f8`; the now-upstream `0.250.0` bump was dropped
  during rebase, and a fresh SDK bump to `0.251.0` was applied. Local
  validation passed again: `python3 tools/scripts/test_audit_top_level.py`
  10/10, `python3 tools/audit.py --`, `python3
  tools/scripts/skill_sync_check.py --base origin/main --head HEAD --mode
  report`, `python3 tools/scripts/version_bump_check.py --base origin/main
  --config tools/scripts/versioning.json --mode=report
  --require-bump-for-fix-feat --accept-intent-trailers` for SDK `0.251.0`,
  Release/GPU-off configure and build of `pulp-test-osc`,
  `pulp-test-osc-channel`, `pulp-test-events`, and `pulp-test-ipc`,
  `ctest --test-dir build -R '^(OscChannel|OSC|IPC|EventLoop)'
  --output-on-failure` 132/132, Release flag verification for
  `pulp-test-osc`, `tools/check-docs.sh` with the existing 76 warnings, and
  diff hygiene.
- `tools/scripts/local_diff_cover.sh` hits the local GPU/Skia configure gate in
  this worktree, so the same coverage pipeline was run manually in
  `build-cov` with `PULP_ENABLE_COVERAGE=ON`, `PULP_ENABLE_GPU=OFF`, the OSC
  targets, `LLVM_PROFILE_FILE`, and the same 75% diff-coverage floor. The
  focused coverage CTest run passed 100/100. Result: 94% total diff coverage
  (`core/osc/src/bundle.cpp` 100%, `core/osc/src/osc_udp.cpp` 94.3%). The
  remaining uncovered diff lines are defensive socket cleanup paths for
  unresolved addrinfo, `getsockname()` failure, and receive-timeout
  `setsockopt()` failure that are not deterministic to force through the public
  API; GitHub Codecov remains the authoritative PR-side coverage result after
  the refreshed push.
- A fresh attempt to run the stock target-limited local diff-cover wrapper
  failed before coverage collection at the same local GPU/Skia design-tool
  configure gate. The focused local test coverage above is green; the
  refreshed GitHub Codecov result remains the merge gate.
- Final RepoPrompt and Claude blocker reviews after the rebase and coverage
  sweep found no P0/P1 correctness issues in the branch implementation or the
  pending OSC test/docs updates.

## Suggested Order

1. Threads/processes closure in PR #2815. Complete.
2. Main-thread dispatcher in PR #2825. Complete.
3. OSC bundle-aware send/receive, listener filtering, and format-error
   callbacks. Active.
4. Non-Apple native embedding implementations or explicit supported-platform
   contracts.
5. Larger follow-up audit for audio formats, audio device management, and any
   remaining platform capability claims.

The first three are relatively contained and can turn partial support into
clear, testable guarantees. The latter work touches broader platform contracts
and should be designed as multi-slice work.

## Exit Criteria

- Public docs can state exactly which formats are readable and writable in the
  current build.
- OSC supports messages and bundles through public sender/receiver APIs.
- There is one canonical child-process API with tested timeout and cancellation.
- Applications can manage audio device setup through a single persisted manager.
- UI work can be marshalled to the platform main thread through a documented API.
- Native child embedding is implemented, tested, or explicitly marked unsupported
  per platform.
