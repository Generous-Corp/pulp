# Platform Capability Closure Plan

**Date:** 2026-05-25
**Status:** Planning
**Scope:** Runtime, events, audio I/O, audio formats, window embedding, OSC

## Summary

Pulp already has real implementation in each audited capability area, but the
support is uneven. The strongest foundations are child-process launching,
thread-backed task dispatch, platform audio I/O backends, WAV/AIFF/FLAC/MP3/Ogg
read paths, and OSC message transport. The main gaps are higher-level manager
APIs, native main-thread dispatch, complete read/write coverage, concrete
non-Apple native embedding, and bundle-aware OSC routing.

This plan is code-grounded. It treats existing implementation files as the
source of truth and avoids making product claims based only on planning docs.

## Execution Model

This report is the living tracker for the first closure pass. Each feature
track should update this file in its own branch with the final scope,
implementation notes, tests, coverage proof, and PR link before shipping.

| Track | Branch target | Worktree target | Status | Done means |
| --- | --- | --- | --- | --- |
| Threads and processes | `feature/platform-threads-processes` | `pulp-platform-threads-processes` | PR #2815 open; fixing review/CI findings | Canonical platform process surface, runtime blocking wrapper, tested launch/wait/cancel/output/IPC behavior, no unneeded current-process or timer additions |
| Native event loop | `feature/platform-main-thread-dispatch` | `pulp-platform-main-thread-dispatch` | Queued | Cross-platform main-thread dispatcher contract, platform registrations where available, sync/async dispatch tests, EventLoop thread-id race fixed |
| OSC | `feature/platform-osc-bundles-routing` | `pulp-platform-osc-bundles-routing` | Queued | Typed bundle send/receive, listener filtering using existing address matching, invalid-packet error callback, focused UDP and pure parser tests |
| Native windows | `feature/platform-native-window-embedding` | `pulp-platform-native-window-embedding` | Queued | First-party non-Apple host/plugin embedding path or explicit supported-platform contract, child attach/bounds/detach tests, docs updated to avoid overclaiming |

Validation expectations for each PR:
- Add or update focused unit tests for every new public behavior.
- Run the smallest relevant CTest target during iteration, then broader
  validation before shipping.
- Run local diff coverage via `tools/scripts/local_diff_cover.sh` when the PR
  changes coverage-bearing C/C++ sources. The CI diff-coverage floor is 75%.
- Sweep the full local diff for correctness, lifecycle, cross-platform, and
  docs issues before opening the PR; use Claude as an independent review pass
  and fix actionable findings before submitting.
- Use the normal Shipyard PR flow so required checks and Codecov comments are
  recorded before merge.
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
| OSC | Partially implemented | `core/osc/include/pulp/osc/osc.hpp`, `core/osc/include/pulp/osc/bundle.hpp`, `core/osc/src/osc.cpp`, `core/osc/src/osc_udp.cpp`, `core/osc/src/bundle.cpp`, `core/osc/src/osc_channel.cpp`, `test/test_osc_channel.cpp` |

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

## Suggested Order

1. JACK `DeviceInfo` reconciliation and AIFC compression whitelist.
2. Audio format capability reporting and tests.
3. OSC bundle-aware send/receive and format-error callbacks.
4. Process API cleanup, including the `ConnectedChildProcess` layering decision.
5. Audio device manager MVP.
6. Main-thread dispatcher.
7. Non-Apple native embedding implementations.

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
