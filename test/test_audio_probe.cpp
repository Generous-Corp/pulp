// test_audio_probe.cpp — Phase 5 RT-probe acceptance.
//
// GATE 2: an allocation-counting test (RtAllocationProbe) asserting ZERO
// allocations across N analyze_output() calls after prepare(). Plus correctness
// for peak/RMS/clip/NaN/silence counting, snapshot sequence monotonicity +
// stale/drop detection, TripleBuffer publish/read coherence, the capture
// AbstractFifo's drop accounting, AudioStats device-counter mirroring, and the
// complaint-oriented boundary report.

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/audio_boundary_report.hpp>
#include <pulp/audio/audio_probe.hpp>
#include <pulp/audio/audio_probe_snapshot.hpp>
#include <pulp/audio/audio_stats.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace pulp::audio;

namespace {

// A fixed-capacity stereo block backed by stable storage. Channel pointers are
// allocated ONCE so building the view never allocates on the audio path.
struct StereoBlock {
    std::vector<float> left;
    std::vector<float> right;
    std::vector<const float*> ptrs;

    explicit StereoBlock(std::size_t frames)
        : left(frames, 0.0f), right(frames, 0.0f) {
        ptrs = {left.data(), right.data()};
    }

    BufferView<const float> view() const {
        return BufferView<const float>(ptrs.data(), 2, left.size());
    }

    void fill(float l, float r) {
        std::fill(left.begin(), left.end(), l);
        std::fill(right.begin(), right.end(), r);
    }
};

}  // namespace

TEST_CASE("AudioProbe::analyze_output does not allocate after prepare",
          "[rt-safety][audio-probe][issue-290]") {
    AudioProbe probe;
    probe.prepare(/*max_channels=*/2, /*max_frames=*/256, /*sample_rate=*/48000.0);

    constexpr std::size_t kFrames = 256;
    StereoBlock block(kFrames);
    // A non-trivial signal so every counting branch runs: clipping, a NaN, and
    // ordinary samples — exercising the widest RT path inside the no-alloc scope.
    block.fill(0.5f, -0.5f);
    block.left[10] = 2.0f;   // clip
    block.right[20] = std::numeric_limits<float>::quiet_NaN();  // nan

    const auto v = block.view();

    std::size_t allocation_count = 0;
    std::size_t allocated_bytes = 0;
    {
        pulp::test::RtAllocationProbe alloc_probe;
        pulp::runtime::ScopedNoAlloc no_alloc;
        for (int i = 0; i < 1000; ++i) {
            probe.analyze_output(v);
        }
        allocation_count = alloc_probe.allocation_count();
        allocated_bytes = alloc_probe.allocated_bytes();
    }

    REQUIRE(allocation_count == 0);
    REQUIRE(allocated_bytes == 0);
}

TEST_CASE("AudioProbe::analyze_output with capture ring does not allocate",
          "[rt-safety][audio-probe][issue-290]") {
    AudioProbe probe;
    AudioProbe::CaptureConfig cap;
    cap.capture_frames = 1024;
    probe.prepare(2, 256, 48000.0, AudioProbeStage::kStandaloneOutputBoundary, cap);
    REQUIRE(probe.capture_enabled());

    StereoBlock block(256);
    block.fill(0.25f, 0.25f);
    const auto v = block.view();

    std::size_t allocation_count = 0;
    {
        pulp::test::RtAllocationProbe alloc_probe;
        pulp::runtime::ScopedNoAlloc no_alloc;
        for (int i = 0; i < 500; ++i) probe.analyze_output(v);
        allocation_count = alloc_probe.allocation_count();
    }
    REQUIRE(allocation_count == 0);
}

TEST_CASE("AudioProbe counts peak, RMS, clip, NaN, silence correctly",
          "[audio-probe]") {
    AudioProbe probe;
    probe.prepare(2, 64, 48000.0);

    SECTION("known DC level → exact peak and RMS") {
        StereoBlock block(64);
        block.fill(0.5f, -0.25f);
        probe.analyze_output(block.view());
        const auto snap = probe.latest();
        REQUIRE(snap.channel_count == 2);
        REQUIRE(snap.block_size == 64);
        REQUIRE_THAT(snap.peak[0], WithinAbs(0.5f, 1e-6f));
        REQUIRE_THAT(snap.peak[1], WithinAbs(0.25f, 1e-6f));
        REQUIRE_THAT(snap.rms[0], WithinAbs(0.5f, 1e-5f));
        REQUIRE_THAT(snap.rms[1], WithinAbs(0.25f, 1e-5f));
        REQUIRE_THAT(snap.peak_max, WithinAbs(0.5f, 1e-6f));
        REQUIRE(snap.clip_count == 0);
        REQUIRE(snap.nan_inf_count == 0);
        REQUIRE(snap.silence_run_blocks == 0);  // 0.5 > silence threshold
    }

    SECTION("clipping is counted per-sample, not per-block") {
        StereoBlock block(64);
        block.fill(0.1f, 0.1f);
        block.left[0] = 1.5f;   // clip
        block.left[1] = -2.0f;  // clip
        probe.analyze_output(block.view());
        const auto snap = probe.latest();
        REQUIRE(snap.clip_count == 2);
    }

    SECTION("NaN and Inf are counted and excluded from peak/RMS") {
        StereoBlock block(64);
        block.fill(0.0f, 0.0f);
        block.left[0] = std::numeric_limits<float>::quiet_NaN();
        block.left[1] = std::numeric_limits<float>::infinity();
        probe.analyze_output(block.view());
        const auto snap = probe.latest();
        REQUIRE(snap.nan_inf_count == 2);
        REQUIRE_THAT(snap.peak[0], WithinAbs(0.0f, 1e-6f));  // non-finite skipped
    }

    SECTION("silence run accumulates then resets on signal") {
        StereoBlock quiet(64);
        quiet.fill(0.0f, 0.0f);
        probe.analyze_output(quiet.view());
        probe.analyze_output(quiet.view());
        probe.analyze_output(quiet.view());
        REQUIRE(probe.latest().silence_run_blocks == 3);

        StereoBlock loud(64);
        loud.fill(0.3f, 0.3f);
        probe.analyze_output(loud.view());
        REQUIRE(probe.latest().silence_run_blocks == 0);
    }
}

TEST_CASE("AudioProbe snapshot sequence number is monotonic and gaps detectable",
          "[audio-probe]") {
    AudioProbe probe;
    probe.prepare(1, 32, 44100.0);

    StereoBlock block(32);
    block.fill(0.2f, 0.2f);

    // prepare() publishes sequence 0; the first analyze_output() makes it 1.
    REQUIRE(probe.latest().sequence_number == 0);

    std::uint64_t last = 0;
    for (int i = 0; i < 10; ++i) {
        probe.analyze_output(block.view());
        const auto snap = probe.latest();
        REQUIRE(snap.sequence_number > last);  // strictly monotonic
        last = snap.sequence_number;
    }

    // TripleBuffer is latest-wins: producing several blocks between reads
    // advances the sequence by more than one — a reader sees the gap.
    const std::uint64_t before = last;
    probe.analyze_output(block.view());
    probe.analyze_output(block.view());
    probe.analyze_output(block.view());
    const auto after = probe.latest();
    REQUIRE(after.sequence_number == before + 3);  // gap of 3 is visible
}

TEST_CASE("AudioProbe TripleBuffer publish/read coherence",
          "[audio-probe]") {
    AudioProbe probe;
    probe.prepare(2, 16, 48000.0);

    StereoBlock a(16);
    a.fill(0.1f, 0.1f);
    StereoBlock b(16);
    b.fill(0.8f, 0.8f);

    probe.analyze_output(a.view());
    const auto snap_a = probe.latest();
    REQUIRE_THAT(snap_a.peak_max, WithinAbs(0.1f, 1e-6f));

    probe.analyze_output(b.view());
    const auto snap_b = probe.latest();
    REQUIRE_THAT(snap_b.peak_max, WithinAbs(0.8f, 1e-6f));

    // Re-reading without a new write returns the same coherent value.
    const auto snap_b2 = probe.latest();
    REQUIRE(snap_b2.sequence_number == snap_b.sequence_number);
    REQUIRE_THAT(snap_b2.peak_max, WithinAbs(0.8f, 1e-6f));
}

TEST_CASE("AudioProbe capture ring drains and accounts for drops",
          "[audio-probe]") {
    AudioProbe probe;
    AudioProbe::CaptureConfig cap;
    cap.capture_frames = 100;  // small ring to force overflow
    probe.prepare(1, 64, 48000.0, AudioProbeStage::kStandaloneOutputBoundary, cap);

    StereoBlock block(64);
    block.fill(0.5f, 0.5f);

    // First block (64 frames) fits in the 100-frame ring.
    probe.analyze_output(block.view());
    REQUIRE(probe.latest().dropped_capture_frames == 0);

    // Second block: only 36 of 64 fit before the ring is full → 28 dropped,
    // counted (never silently lost).
    probe.analyze_output(block.view());
    REQUIRE(probe.latest().dropped_capture_frames == 28);

    // Consumer drains everything available.
    std::vector<float> out(200, -1.0f);
    const int read = probe.read_capture(out.data(), static_cast<int>(out.size()));
    REQUIRE(read == 100);  // ring held its full capacity
    for (int i = 0; i < read; ++i)
        REQUIRE_THAT(out[static_cast<std::size_t>(i)], WithinAbs(0.5f, 1e-6f));

    // After draining, new writes fit again.
    probe.analyze_output(block.view());
    REQUIRE(probe.latest().dropped_capture_frames == 28);  // unchanged: this block fit
}

TEST_CASE("AudioStats mirrors device counters without shadowing",
          "[audio-probe][audio-stats]") {
    // The probe owns signal-content counters and NEVER touches device_xruns /
    // cpu_overloads — those are mirrors the host fills from the device owner.
    AudioProbe probe;
    probe.prepare(2, 64, 48000.0);

    StereoBlock block(64);
    block.fill(0.1f, 0.1f);
    block.left[0] = 2.0f;  // clip
    block.left[1] = std::numeric_limits<float>::quiet_NaN();
    probe.analyze_output(block.view());

    AudioStats s = probe.stats();
    REQUIRE(s.callbacks == 1);
    REQUIRE(s.nan_blocks >= 1);
    REQUIRE(s.clipped_blocks >= 1);
    // Probe leaves device mirrors at zero — the host populates them.
    REQUIRE(s.device_xruns == 0);
    REQUIRE(s.cpu_overloads == 0);

    // The host mirrors device-owned counters in afterward; the probe call
    // above did not invent them.
    s.device_xruns = 7;      // e.g. AudioDeviceManager::xrun_count()
    s.cpu_overloads = 3;     // e.g. CoreAudio overload listener
    REQUIRE(s.device_xruns == 7);
    REQUIRE(s.cpu_overloads == 3);
    // Re-querying the probe still reports zero — it does not shadow the host's
    // mirror values.
    REQUIRE(probe.stats().device_xruns == 0);
}

TEST_CASE("AudioProbe reset clears cumulative counters",
          "[audio-probe]") {
    AudioProbe probe;
    probe.prepare(1, 32, 48000.0);
    StereoBlock block(32);
    block.fill(2.0f, 2.0f);  // clips every sample
    probe.analyze_output(block.view());
    REQUIRE(probe.latest().clip_count > 0);
    probe.reset();
    probe.analyze_output(StereoBlock(32).view());  // silence
    const auto snap = probe.latest();
    REQUIRE(snap.clip_count == 0);
    REQUIRE(snap.callbacks == 1);
}

TEST_CASE("AudioProbeSnapshot is a trivially-copyable shared schema",
          "[audio-probe]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<AudioProbeSnapshot>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<AudioStats>);
}

// ── Complaint-oriented boundary report ─────────────────────────────────────

namespace {

AudioProbeSnapshot make_snap(AudioProbeStage stage, float peak_max,
                             std::uint64_t silence_run) {
    AudioProbeSnapshot s{};
    s.stage_id = stage;
    s.peak_max = peak_max;
    s.silence_run_blocks = silence_run;
    s.sequence_number = 1;
    return s;
}

}  // namespace

TEST_CASE("Boundary report distinguishes processor / boundary / device faults",
          "[audio-probe][boundary-report]") {
    SECTION("processor silent → processor diagnosis") {
        BoundaryReportInputs in;
        in.processor_output =
            make_snap(AudioProbeStage::kProcessorOutput, 0.0f, 40);
        in.standalone_boundary =
            make_snap(AudioProbeStage::kStandaloneOutputBoundary, 0.0f, 40);
        const auto report = build_boundary_report(in);
        REQUIRE(report.diagnosis == BoundaryDiagnosis::kProcessorSilent);
        REQUIRE(report.text.find("processor rendered silence") != std::string::npos);
    }

    SECTION("processor signal but boundary silent → boundary diagnosis") {
        BoundaryReportInputs in;
        in.processor_output =
            make_snap(AudioProbeStage::kProcessorOutput, 0.3f, 0);
        in.standalone_boundary =
            make_snap(AudioProbeStage::kStandaloneOutputBoundary, 0.0f, 38);
        const auto report = build_boundary_report(in);
        REQUIRE(report.diagnosis == BoundaryDiagnosis::kStandaloneBoundarySilent);
        REQUIRE(report.text.find("output buffer not copied") != std::string::npos);
    }

    SECTION("signal present but device xruns → device diagnosis") {
        BoundaryReportInputs in;
        in.processor_output =
            make_snap(AudioProbeStage::kProcessorOutput, 0.3f, 0);
        in.standalone_boundary =
            make_snap(AudioProbeStage::kStandaloneOutputBoundary, 0.3f, 0);
        DeviceStatsView dev;
        dev.callback_running = true;
        dev.sample_rate = 48000.0;
        dev.buffer_size = 128;
        dev.xruns = 12;
        in.device = dev;
        const auto report = build_boundary_report(in);
        REQUIRE(report.diagnosis == BoundaryDiagnosis::kDeviceProblem);
        REQUIRE(report.text.find("12 xruns") != std::string::npos);
    }

    SECTION("everything healthy → signal present") {
        BoundaryReportInputs in;
        in.processor_output =
            make_snap(AudioProbeStage::kProcessorOutput, 0.3f, 0);
        in.standalone_boundary =
            make_snap(AudioProbeStage::kStandaloneOutputBoundary, 0.3f, 0);
        DeviceStatsView dev;
        dev.callback_running = true;
        dev.sample_rate = 48000.0;
        dev.buffer_size = 128;
        dev.xruns = 0;
        in.device = dev;
        const auto report = build_boundary_report(in);
        REQUIRE(report.diagnosis == BoundaryDiagnosis::kSignalPresent);
    }

    SECTION("missing stage reported as no-probe, not faked silence") {
        BoundaryReportInputs in;  // nothing populated
        const auto report = build_boundary_report(in);
        REQUIRE(report.diagnosis == BoundaryDiagnosis::kNoProbeData);
        REQUIRE(report.text.find("Processor output: no probe") != std::string::npos);
        REQUIRE(report.text.find("Standalone boundary: no probe") != std::string::npos);
    }
}
