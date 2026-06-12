#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/sample_resource.hpp>

using namespace pulp::audio;

namespace {

AudioFileData make_sample(uint32_t channels, uint64_t frames, uint32_t sample_rate = 48000) {
    AudioFileData data;
    data.sample_rate = sample_rate;
    data.channels.resize(channels);
    for (uint32_t ch = 0; ch < channels; ++ch) {
        data.channels[ch].resize(static_cast<std::size_t>(frames));
        for (uint64_t i = 0; i < frames; ++i) {
            data.channels[ch][static_cast<std::size_t>(i)] =
                static_cast<float>(ch + 1) * 0.1f + static_cast<float>(i) * 0.001f;
        }
    }
    return data;
}

} // namespace

TEST_CASE("SampleResourceHandle publishes loaded sample snapshots",
          "[audio][sample-resource][phase3]") {
    SampleResourceHandle handle;
    REQUIRE_FALSE(handle.snapshot().ready());

    auto data = make_sample(2, 16, 44100);
    REQUIRE(handle.publish_loaded(std::move(data), "kick.wav", 1024));

    const auto snap = handle.snapshot();
    REQUIRE(snap.status == SampleResourceStatus::Loaded);
    REQUIRE(snap.ready());
    REQUIRE(snap.generation == 1);
    REQUIRE(snap.channel_count == 2);
    REQUIRE(snap.frame_count == 16);
    REQUIRE(snap.sample_rate == 44100);
    REQUIRE(snap.byte_size == 2 * 16 * sizeof(float));
    REQUIRE(snap.memory_budget_bytes == 1024);
    REQUIRE(snap.data != nullptr);
    REQUIRE(snap.data->channels[1][3] > snap.data->channels[0][3]);

    const auto diag = handle.diagnostics();
    REQUIRE(diag.status == SampleResourceStatus::Loaded);
    REQUIRE(diag.path == "kick.wav");
    REQUIRE(diag.reason.empty());
    REQUIRE(diag.byte_size == snap.byte_size);
}

TEST_CASE("SampleResourceHandle records missing resources without sample data",
          "[audio][sample-resource][missing][phase3]") {
    SampleResourceHandle handle;
    handle.publish_missing("missing/snare.wav", "file not found");

    const auto snap = handle.snapshot();
    REQUIRE(snap.status == SampleResourceStatus::Missing);
    REQUIRE_FALSE(snap.ready());
    REQUIRE(snap.generation == 1);
    REQUIRE(snap.data == nullptr);

    const auto diag = handle.diagnostics();
    REQUIRE(diag.status == SampleResourceStatus::Missing);
    REQUIRE(diag.path == "missing/snare.wav");
    REQUIRE(diag.reason == "file not found");
}

TEST_CASE("SampleResourceHandle rejects decoded samples over memory budget",
          "[audio][sample-resource][budget][phase3]") {
    SampleResourceHandle handle;
    auto data = make_sample(2, 64);
    const auto bytes = SampleResourceHandle::decoded_byte_size(data);

    REQUIRE_FALSE(handle.publish_loaded(std::move(data), "oversized.wav", bytes - 1));

    const auto snap = handle.snapshot();
    REQUIRE(snap.status == SampleResourceStatus::Oversized);
    REQUIRE_FALSE(snap.ready());
    REQUIRE(snap.byte_size == bytes);
    REQUIRE(snap.memory_budget_bytes == bytes - 1);
    REQUIRE(snap.data == nullptr);

    const auto diag = handle.diagnostics();
    REQUIRE(diag.status == SampleResourceStatus::Oversized);
    REQUIRE(diag.path == "oversized.wav");
    REQUIRE(diag.reason == "decoded sample exceeds memory budget");
}

TEST_CASE("SampleResourceHandle relink advances generation and replaces state",
          "[audio][sample-resource][relink][phase3]") {
    SampleResourceHandle handle;
    handle.publish_missing("old.wav", "file not found");
    const auto missing = handle.snapshot();

    auto data = make_sample(1, 8);
    REQUIRE(handle.publish_loaded(std::move(data), "new.wav"));
    const auto loaded = handle.snapshot();

    REQUIRE(missing.generation == 1);
    REQUIRE(loaded.generation == 2);
    REQUIRE(loaded.status == SampleResourceStatus::Loaded);
    REQUIRE(loaded.ready());
    REQUIRE(loaded.channel_count == 1);
    REQUIRE(loaded.frame_count == 8);
    REQUIRE(handle.diagnostics().path == "new.wav");
}

TEST_CASE("SampleResourceHandle snapshot is allocation-free after publish",
          "[audio][sample-resource][rt-safety][phase3]") {
    SampleResourceHandle handle;
    auto data = make_sample(2, 32);
    REQUIRE(handle.publish_loaded(std::move(data), "rt.wav"));

    SampleResourceSnapshot snap;
    {
        pulp::test::RtAllocationProbe probe;
        snap = handle.snapshot();
        REQUIRE_FALSE(probe.saw_allocation());
    }

    REQUIRE(snap.ready());
    REQUIRE(snap.data != nullptr);
    REQUIRE(snap.frame_count == 32);
}
