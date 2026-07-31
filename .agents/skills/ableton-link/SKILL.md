---
name: ableton-link
description: Configure, implement, and test Pulp's optional desktop Ableton Link tempo-sync adapter while preserving the developer-supplied SDK, licensing, realtime, latency-compensation, and no-install boundaries.
---

# Ableton Link

Use this skill for Pulp's desktop `Ableton/link` integration, its CMake opt-in,
or the backend-neutral tempo-sync seam in `core/playback`.

## Scope and licensing boundary

- This is the desktop C++ `Ableton/link` SDK. It is not iOS LinkKit and not
  `m4l-connection-kit`. LinkKit needs a separate licensing decision, option,
  SDK path, and implementation.
- Link is available under GPLv2+ or a proprietary arrangement with Ableton.
  Pulp's MIT license grants no Link rights. The developer enabling the adapter
  is responsible for having suitable rights.
- Pulp never fetches, bundles, installs, exports, or redistributes Link. Keep
  the SDK outside the Pulp source tree. Do not add it to `DEPENDENCIES.md` or
  `NOTICE.md` because Pulp ships none of it.
- The portable `pulp::playback` target owns only Pulp code. The optional
  `pulp::ableton-link` target exists only in a source build with the SDK enabled
  and must never enter `cmake --install` or Pulp's exported target closure. Its
  adapter declaration lives under `core/playback/adapters/include`, exposed
  only through that target's `BUILD_INTERFACE`; do not place it under the
  installed `core/playback/include` tree.

## Obtain and enable the desktop SDK

Clone recursively to an absolute path outside the checkout; Link requires its
ASIO submodule even on platforms that do not use an ASIO audio device:

```bash
git clone --recurse-submodules https://github.com/Ableton/link.git /absolute/path/to/link

cmake -S . -B build-link -DCMAKE_BUILD_TYPE=Release \
  -DPULP_ENABLE_ABLETON_LINK=ON \
  -DPULP_ABLETON_LINK_SDK_DIR=/absolute/path/to/link
```

`PULP_ENABLE_ABLETON_LINK` defaults to `OFF`. When enabled, a missing, partial,
in-tree, or non-desktop SDK path is a configure error. The SDK path can also
come from the `PULP_ABLETON_LINK_SDK_DIR` environment variable, but an explicit
CMake cache value is easier to audit.

## Runtime contract

- `TempoSyncSource` is Pulp's backend-neutral, audio-thread interface. Keep
  Link types and Link-specific peer/enable/start-stop controls out of it.
- `AbletonLinkTempoSync` owns those Link-specific controls. Call `set_enabled`
  and `set_start_stop_sync_enabled` from a control thread, never from the audio
  callback. `output_host_time` and `capture_audio_block` are realtime-safe.
- Call `output_host_time(output_latency_frames, sample_rate, time)` in the audio
  callback, then pass its opaque `TempoSyncHostTime` to `MasterTransport`. The
  adapter reads Link's realtime clock and adds device/output latency so the
  timestamp names the first sample at the output boundary. A callback timestamp
  without compensation produces a stable audible offset.
- `TempoSyncHostTime` is tagged with its producing source. A transport rejects
  a default token or one from another source before asking its backend to
  capture a block, preventing Link, device, and wall-clock epochs from mixing.
- The public desktop adapter models APIs that report output latency from the
  callback. It does not accept an arbitrary device-host timestamp. A platform
  integration whose audio API exposes a calibrated future device timestamp
  needs an explicit Link-clock conversion boundary; do not forward that raw
  timestamp or a jittery wall-clock read as though it were Link time.
- A configured `TempoSyncSource` must outlive `MasterTransport`. Use the
  host-time `begin_block(frame_count, output_host_time, snapshot)`
  overload. The ordinary overload returns `TempoSyncHostTimeRequired`.
- The session mapping is authoritative. Disabled, failed, or invalid sources
  fail the block; they never fall back to the document tempo clock.
- Joining does not send the config's initial tempo, position, or play state.
  Only explicit `set_tempo_sync_tempo`, `seek`, and `set_playing` calls publish
  session commands, which avoids hijacking an existing jam on attach.
- A Link-projected block retains Pulp's fixed one-or-two-range transport
  contract and precise fractional host ticks. `begin_scrub()` rejects an active
  shared tempo source because scrubbing needs a private repositioning clock;
  use an unsynchronized transport for scrub playback.
- Preserve `SessionState::timeForIsPlaying()` in
  `TempoSyncBlockState::is_playing_at_host_time_micros`. A
  `TransportSnapshot` has one playing state for its whole half-open block: a
  start/stop effective at or before the first sample applies immediately, while
  one inside or at/after the block end is deterministically deferred until a
  later block starts at or beyond that timestamp. Do not flatten the timestamp
  into `SessionState::isPlaying()` or claim sample-accurate start/stop splitting
  that the snapshot contract cannot represent.

## Verification

The SDK-independent interface tests are the public-CI gate. The SDK-present
test is always registered: a stock build exits 77 and CTest reports `Skipped`
with the enablement flags, while an enabled local build executes the real
adapter.

```bash
cmake --build build --target \
  pulp-test-playback-tempo-sync \
  pulp-test-ableton-link-sdk \
  timeline-program-threadless-no-exceptions-check

build/test/pulp-test-playback-tempo-sync --reporter compact
ctest --test-dir build -V -R '^playback-ableton-link-sdk-present$'

cmake --build build-link --target \
  pulp-test-playback-tempo-sync pulp-test-ableton-link-sdk
ctest --test-dir build-link -V -R '^playback-ableton-link-sdk-present$'
```

Check the interface assertion count; a filtered CTest command can match
nothing. For substantive changes, prove negative controls for fail-closed
source errors and precise beat projection, then restore the implementation.

When the portable interface or transport integration changes, also update the
`playback` skill and keep `tempo_sync.cpp` in the native, threadless, WAM, and
WebCLAP playback source closure. Keep the SDK adapter under
`core/playback/adapters/`, outside the portable `src/` closure.
