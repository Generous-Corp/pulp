#pragma once

#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/loop_types.hpp>
#include <pulp/audio/published_sample_store.hpp>
#include <pulp/audio/sample_asset.hpp>
#include <pulp/audio/sample_stream_loop_voice_reader.hpp>
#include <pulp/audio/sample_stream_voice_reader.hpp>
#include <pulp/signal/adsr.hpp>

#include "sampler_mip_pyramid.hpp"

#include <cstdint>

namespace pulp::examples {

/// A single voice for polyphonic sample playback.
struct SamplerVoice {
    bool active = false;
    int note = -1;
    float velocity = 0.0f;
    signal::Adsr adsr;
    audio::LoopRenderer renderer;
    audio::PublishedSampleView sample;
    SamplerMipLevelView resident_mip;
    audio::SampleAssetView streamed_asset;
    audio::SampleStreamLoopVoiceReader stream_reader;
    audio::SampleStreamLoopVoiceReader lookahead_reader;
    audio::SampleStreamLoopBlockPlan pending_lookahead{};
    audio::SampleStreamRequesterToken requester{};
    std::uint64_t selection_generation = 0;
    std::uint32_t pending_demand_index = 0;
    std::uint32_t pending_refresh_index = 0;
    std::uint32_t stream_boundary_demand_index = 0;
    double stream_playback_rate = 0.0;
    double lookahead_lead_source_frames = 0.0;
    bool streamed = false;
    bool stream_attack_pending = false;
    bool stream_boundary_pending = false;
    bool pending_lookahead_valid = false;
    bool released = false;

    void reset() {
        active = false;
        note = -1;
        velocity = 0.0f;
        sample = {};
        resident_mip = {};
        streamed_asset = {};
        stream_reader.reset();
        lookahead_reader.reset();
        pending_lookahead = {};
        requester = {};
        selection_generation = 0;
        pending_demand_index = 0;
        pending_refresh_index = 0;
        stream_boundary_demand_index = 0;
        stream_playback_rate = 0.0;
        lookahead_lead_source_frames = 0.0;
        streamed = false;
        stream_attack_pending = false;
        stream_boundary_pending = false;
        pending_lookahead_valid = false;
        released = false;
        adsr.reset();
        renderer.reset();
    }

    bool start(int n,
               float vel,
               double speed,
               float host_sample_rate,
               const audio::PublishedSampleView& sample_view,
               const SamplerMipLevelView& mip_view,
               const audio::LoopRegion& region,
               std::uint64_t source_frames,
               const audio::PreparedSampleInterpolation& interpolation) {
        reset();
        if (!renderer.set_region(region, source_frames)) return false;
        if (!renderer.set_interpolation(interpolation)) return false;
        note = n;
        velocity = vel;
        sample = sample_view;
        resident_mip = mip_view;
        active = true;
        adsr.set_sample_rate(host_sample_rate);
        adsr.note_on();
        renderer.set_playback_rate(speed);
        renderer.start();
        return true;
    }

    bool start_streamed(int n,
                        float vel,
                        float host_sample_rate,
                        const audio::SampleAssetView& asset_view,
                        const audio::LoopRegion& region,
                        double playback_rate,
                        const audio::PreparedSampleInterpolation& interpolation,
                        audio::SampleStreamRequesterToken requester_token,
                        std::uint64_t published_generation) {
        reset();
        if (!stream_reader.prepare(asset_view, requester_token, region,
                                   playback_rate, interpolation) ||
            !lookahead_reader.prepare(asset_view, requester_token, region,
                                      playback_rate, interpolation)) {
            reset();
            return false;
        }
        note = n;
        velocity = vel;
        streamed_asset = asset_view;
        requester = requester_token;
        stream_playback_rate = playback_rate;
        selection_generation = published_generation;
        streamed = true;
        stream_attack_pending = region.reverse_entry ||
            region.playback_mode == audio::LoopPlaybackMode::ReverseOnce;
        stream_boundary_pending = !stream_attack_pending &&
            !asset_view.fully_resident();
        active = true;
        adsr.set_sample_rate(host_sample_rate);
        adsr.note_on();
        return true;
    }

    void release() {
        adsr.note_off();
        released = true;
    }
};

class SamplerSampleStore : public audio::PublishedSampleStore {
public:
    static constexpr std::uint32_t kSlotCount = 2;
    static constexpr std::uint32_t kMaxChannels = 2;
    static constexpr std::uint64_t kMaxFrames = 48000ull * 60ull;

    bool prepare() {
        return audio::PublishedSampleStore::prepare(
            audio::PublishedSampleStoreConfig{kSlotCount, kMaxChannels, kMaxFrames});
    }
};

}  // namespace pulp::examples
