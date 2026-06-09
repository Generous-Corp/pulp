#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_stream_handoff.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/planar_audio_ring_buffer.hpp>
#include <pulp/audio/published_sample_store.hpp>
#include <pulp/audio/realtime_sample_recorder.hpp>
#include <pulp/audio/rolling_audio_capture_buffer.hpp>
#include <pulp/audio/sample_slot_bank.hpp>
#include <pulp/audio/sample_slot_materializer.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <cstdint>
#include <cmath>
#include <utility>
#include <vector>

using pulp::audio::AudioStreamHandoff;
using pulp::audio::AudioStreamHandoffConfig;
using pulp::audio::AudioStreamHandoffPullResult;
using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::PlanarAudioRingBuffer;
using pulp::audio::PublishedSampleView;
using pulp::audio::PublishedSampleStore;
using pulp::audio::PublishedSampleStoreConfig;
using pulp::audio::RealtimeSampleRecorder;
using pulp::audio::RealtimeSampleRecorderCommand;
using pulp::audio::RealtimeSampleRecorderCommandType;
using pulp::audio::RealtimeSampleRecorderConfig;
using pulp::audio::RealtimeSampleRecorderEvent;
using pulp::audio::RealtimeSampleRecorderEventType;
using pulp::audio::RealtimeSampleRecorderState;
using pulp::audio::RealtimeSampleRecorderTimingType;
using pulp::audio::RollingAudioCaptureBuffer;
using pulp::audio::RollingAudioCaptureBufferConfig;
using pulp::audio::RollingAudioCaptureMaterializeStatus;
using pulp::audio::SampleSlotBank;
using pulp::audio::SampleSlotState;
using pulp::audio::materialize_completed_recording_to_slot;
using pulp::audio::publish_completed_recording_to_slot;

namespace {

BufferView<const float> const_view(const Buffer<float>& buffer,
                                   std::vector<const float*>& ptrs) {
    ptrs.resize(buffer.num_channels());
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        ptrs[ch] = buffer.channel(ch).data();
    }
    return {ptrs.data(), buffer.num_channels(), buffer.num_samples()};
}

void fill_sequence(Buffer<float>& buffer, float first) {
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        for (std::size_t i = 0; i < buffer.num_samples(); ++i) {
            buffer.channel(ch)[i] = first + static_cast<float>(i) +
                                    static_cast<float>(ch) * 1000.0f;
        }
    }
}

}  // namespace

TEST_CASE("SampleSlotBank publishes slots and frees retired slots after ack",
          "[audio][sampler][slots]") {
    SampleSlotBank bank;
    REQUIRE(bank.prepare(2, 2, 4));

    Buffer<float> first(2, 4);
    fill_sequence(first, 1.0f);
    std::vector<const float*> first_ptrs;
    const auto slot_a = bank.reserve_slot(2, 4, 48000.0);
    REQUIRE(slot_a >= 0);
    REQUIRE(bank.write_slot(static_cast<std::uint32_t>(slot_a),
                            const_view(first, first_ptrs),
                            4));
    REQUIRE(bank.complete_slot(static_cast<std::uint32_t>(slot_a)));
    REQUIRE(bank.publish_slot(static_cast<std::uint32_t>(slot_a)));

    const auto published_a = bank.read_published_view();
    REQUIRE(published_a.valid);
    REQUIRE(published_a.slot_index == static_cast<std::uint32_t>(slot_a));
    REQUIRE(published_a.generation == 1);
    REQUIRE(bank.slot_state(static_cast<std::uint32_t>(slot_a)) ==
            SampleSlotState::Published);
    REQUIRE(bank.slot_view_valid(published_a));
    REQUIRE(bank.channel_data(published_a, 0)[3] == 4.0f);
    REQUIRE(bank.slot_channel_data_for_test(static_cast<std::uint32_t>(slot_a), 0)[3] == 4.0f);

    Buffer<float> second(1, 4);
    fill_sequence(second, 100.0f);
    std::vector<const float*> second_ptrs;
    const auto slot_b = bank.reserve_slot(1, 4, 48000.0);
    REQUIRE(slot_b >= 0);
    REQUIRE(bank.write_slot(static_cast<std::uint32_t>(slot_b),
                            const_view(second, second_ptrs),
                            4));
    REQUIRE(bank.complete_slot(static_cast<std::uint32_t>(slot_b)));
    REQUIRE(bank.publish_slot(static_cast<std::uint32_t>(slot_b)));

    const auto published_b = bank.read_published_view();
    REQUIRE(published_b.valid);
    REQUIRE(published_b.slot_index == static_cast<std::uint32_t>(slot_b));
    REQUIRE(published_b.generation == 2);
    REQUIRE(bank.slot_state(static_cast<std::uint32_t>(slot_a)) ==
            SampleSlotState::Retired);
    REQUIRE(bank.slot_view_valid(published_a));
    REQUIRE(bank.channel_data(published_a, 0)[3] == 4.0f);
    REQUIRE(bank.slot_view_valid(published_b));
    REQUIRE(bank.reserve_slot(1, 4, 48000.0) == -1);
    REQUIRE_FALSE(bank.acknowledge_audio_generation(1));
    REQUIRE(bank.slot_state(static_cast<std::uint32_t>(slot_a)) ==
            SampleSlotState::Retired);
    REQUIRE(bank.acknowledge_audio_generation(2));
    REQUIRE(bank.slot_state(static_cast<std::uint32_t>(slot_a)) ==
            SampleSlotState::Free);
    REQUIRE_FALSE(bank.slot_view_valid(published_a));
    REQUIRE(bank.channel_data(published_a, 0) == nullptr);
    REQUIRE(bank.reserve_slot(1, 4, 48000.0) == slot_a);

    REQUIRE_FALSE(bank.prepare(0, 2, 4));
    REQUIRE(bank.slot_count() == 0);
    REQUIRE(bank.reserve_slot(1, 4, 48000.0) == -1);
}

TEST_CASE("SampleSlotBank publishes buffers through high-level helper",
          "[audio][sampler][slots]") {
    SampleSlotBank bank;
    REQUIRE(bank.prepare(2, 2, 4));

    Buffer<float> first(1, 4);
    fill_sequence(first, 1.0f);
    std::vector<const float*> first_ptrs;
    REQUIRE(bank.publish_from_buffer(const_view(first, first_ptrs), 4, 48000.0));

    const auto published_a = bank.read_published_view();
    REQUIRE(published_a.valid);
    REQUIRE(published_a.generation == 1);
    REQUIRE(published_a.num_channels == 1);
    REQUIRE(bank.channel_data(published_a, 0)[3] == 4.0f);

    Buffer<float> second(2, 4);
    fill_sequence(second, 100.0f);
    std::vector<const float*> second_ptrs;
    REQUIRE(bank.publish_from_buffer(const_view(second, second_ptrs), 4, 48000.0));

    const auto published_b = bank.read_published_view();
    REQUIRE(published_b.valid);
    REQUIRE(published_b.generation == 2);
    REQUIRE(bank.slot_state(published_a.slot_index) == SampleSlotState::Retired);
    REQUIRE(bank.reserve_slot(1, 4, 48000.0) == -1);

    Buffer<float> third(1, 4);
    fill_sequence(third, 200.0f);
    std::vector<const float*> third_ptrs;
    REQUIRE(bank.publish_from_buffer(const_view(third, third_ptrs),
                                     4,
                                     48000.0,
                                     published_b.generation));

    const auto published_c = bank.read_published_view();
    REQUIRE(published_c.valid);
    REQUIRE(published_c.generation == 3);
    REQUIRE(published_c.slot_index == published_a.slot_index);
    REQUIRE(bank.slot_state(published_c.slot_index) == SampleSlotState::Published);
    REQUIRE_FALSE(bank.slot_view_valid(published_a));
    REQUIRE(bank.slot_state(published_b.slot_index) == SampleSlotState::Retired);
    REQUIRE(bank.slot_view_valid(published_b));
    REQUIRE(bank.acknowledge_audio_generation(published_c.generation));
    REQUIRE_FALSE(bank.slot_view_valid(published_b));

    Buffer<float> invalid(0, 4);
    std::vector<const float*> invalid_ptrs;
    REQUIRE_FALSE(bank.publish_from_buffer(const_view(invalid, invalid_ptrs), 4, 48000.0));
}

TEST_CASE("PublishedSampleStore publishes mono and stereo samples",
          "[audio][sampler][store]") {
    PublishedSampleStore store;
    REQUIRE(store.prepare(PublishedSampleStoreConfig{2, 2, 8}));

    Buffer<float> mono(1, 4);
    fill_sequence(mono, 1.0f);
    REQUIRE(store.load_mono(mono.channel(0).data(), 4, 48000.0));
    REQUIRE(store.has_sample());
    REQUIRE(store.sample_length() == 4);

    auto view = store.read_published_view();
    REQUIRE(view.valid);
    REQUIRE(view.num_channels == 1);
    const float* mono_ptrs[2] = {};
    REQUIRE(store.populate_channel_ptrs(view, mono_ptrs, 2));
    REQUIRE(mono_ptrs[0] != nullptr);
    REQUIRE(mono_ptrs[0][0] == 1.0f);
    REQUIRE(mono_ptrs[0][3] == 4.0f);

    const float interleaved[] = {
        10.0f, 20.0f,
        11.0f, 21.0f,
        12.0f, 22.0f,
    };
    REQUIRE(store.load_interleaved_stereo(interleaved, 3, 48000.0, view.generation));
    view = store.read_published_view();
    REQUIRE(view.valid);
    REQUIRE(view.num_channels == 2);
    REQUIRE(view.num_frames == 3);
    const float* stereo_ptrs[2] = {};
    REQUIRE(store.populate_channel_ptrs(view, stereo_ptrs, 2));
    REQUIRE(stereo_ptrs[0][0] == 10.0f);
    REQUIRE(stereo_ptrs[0][2] == 12.0f);
    REQUIRE(stereo_ptrs[1][0] == 20.0f);
    REQUIRE(stereo_ptrs[1][2] == 22.0f);
}

TEST_CASE("PublishedSampleStore enforces fixed publication capacity",
          "[audio][sampler][store]") {
    PublishedSampleStore store;
    REQUIRE_FALSE(store.prepare(PublishedSampleStoreConfig{}));
    REQUIRE_FALSE(store.prepare(PublishedSampleStoreConfig{1, 1, 4}));
    REQUIRE(store.prepare(PublishedSampleStoreConfig{2, 1, 4}));
    REQUIRE_FALSE(store.prepare(PublishedSampleStoreConfig{3, 1, 4}));

    Buffer<float> source(1, 5);
    fill_sequence(source, 1.0f);
    std::vector<const float*> ptrs;
    REQUIRE_FALSE(store.publish(const_view(source, ptrs), 5, 48000.0));
    REQUIRE_FALSE(store.has_sample());

    Buffer<float> valid(1, 4);
    fill_sequence(valid, 10.0f);
    std::vector<const float*> valid_ptrs;
    REQUIRE(store.publish(const_view(valid, valid_ptrs), 4, 48000.0));
    auto view = store.read_published_view();
    REQUIRE(view.valid);

    const float* out[1] = {};
    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        REQUIRE(store.populate_channel_ptrs(view, out, 1));
    }
    REQUIRE(out[0][0] == 10.0f);
    REQUIRE_FALSE(store.populate_channel_ptrs(view, nullptr, 1));
    REQUIRE_FALSE(store.populate_channel_ptrs(view, out, 0));

    Buffer<float> replacement(1, 3);
    fill_sequence(replacement, 20.0f);
    std::vector<const float*> replacement_ptrs;
    REQUIRE(store.publish(const_view(replacement, replacement_ptrs),
                          3,
                          48000.0,
                          view.generation));
    const auto replacement_view = store.read_published_view();
    REQUIRE(replacement_view.valid);
    REQUIRE(replacement_view.generation == view.generation + 1);
    REQUIRE(replacement_view.num_frames == 3);
    const float* replacement_out[1] = {};
    REQUIRE(store.populate_channel_ptrs(replacement_view, replacement_out, 1));
    REQUIRE(replacement_out[0][0] == 20.0f);
    REQUIRE(replacement_out[0][2] == 22.0f);

    REQUIRE(store.prepare(PublishedSampleStoreConfig{2, 1, 4}));
    REQUIRE_FALSE(store.has_sample());

    store.release();
    REQUIRE_FALSE(store.prepared());
    REQUIRE(store.slot_count() == 0);
    REQUIRE(store.prepare(PublishedSampleStoreConfig{3, 1, 4}));
    REQUIRE(store.slot_count() == 3);
}

TEST_CASE("SampleSlotBank rejects frame-count mismatches on one-shot writes",
          "[audio][sampler][slots]") {
    SampleSlotBank bank;
    REQUIRE(bank.prepare(1, 1, 8));
    REQUIRE(bank.reserve_slot(1, 4, 48000.0) == 0);

    Buffer<float> short_source(1, 3);
    fill_sequence(short_source, 1.0f);
    std::vector<const float*> ptrs;
    REQUIRE_FALSE(bank.write_slot(0, const_view(short_source, ptrs), 4));
    REQUIRE(bank.slot_state(0) == SampleSlotState::Reserved);
    REQUIRE_FALSE(bank.complete_slot(0));
    REQUIRE(bank.release_slot(0));

    REQUIRE(bank.reserve_slot(1, 4, 48000.0) == 0);
    REQUIRE_FALSE(bank.complete_slot(0));
    REQUIRE(bank.release_slot(0));
}

TEST_CASE("SampleSlotBank rejects null source channels on writes",
          "[audio][sampler][slots]") {
    SampleSlotBank bank;
    REQUIRE(bank.prepare(1, 1, 8));
    REQUIRE(bank.reserve_slot(1, 4, 48000.0) == 0);

    std::vector<const float*> ptrs(1, nullptr);
    BufferView<const float> missing_channel(ptrs.data(), 1, 4);
    REQUIRE_FALSE(bank.write_slot(0, missing_channel, 4));
    REQUIRE(bank.slot_state(0) == SampleSlotState::Reserved);
    REQUIRE_FALSE(bank.complete_slot(0));
    REQUIRE(bank.release_slot(0));

    REQUIRE_FALSE(bank.publish_from_buffer(missing_channel, 4, 48000.0));
    REQUIRE_FALSE(bank.read_published_view().valid);
    REQUIRE(bank.slot_state(0) == SampleSlotState::Free);
}

TEST_CASE("SampleSlotBank computes oldest active audio generation",
          "[audio][sampler][slots]") {
    PublishedSampleView current{true, 0, 5, 1, 16, 48000.0};
    PublishedSampleView active[] = {
        {true, 1, 3, 1, 16, 48000.0},
        {false, 0, 0, 0, 0, 0.0},
        {true, 2, 4, 1, 16, 48000.0},
    };

    REQUIRE(SampleSlotBank::oldest_active_generation(current, active, 3) == 3);
    REQUIRE(SampleSlotBank::oldest_active_generation(current, nullptr, 0) == 5);
    REQUIRE(SampleSlotBank::oldest_active_generation({}, active, 3) == 3);
    REQUIRE(SampleSlotBank::oldest_active_generation({}, nullptr, 0) == 0);
}
