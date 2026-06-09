#pragma once

#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/loop_types.hpp>
#include <pulp/audio/published_sample_store.hpp>
#include <pulp/signal/adsr.hpp>

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
    bool released = false;

    void reset() {
        active = false;
        note = -1;
        velocity = 0.0f;
        sample = {};
        released = false;
        adsr.reset();
        renderer.reset();
    }

    bool start(int n,
               float vel,
               double speed,
               float host_sample_rate,
               const audio::PublishedSampleView& sample_view,
               const audio::LoopRegion& region,
               std::uint64_t source_frames) {
        reset();
        if (!renderer.set_region(region, source_frames)) return false;
        note = n;
        velocity = vel;
        sample = sample_view;
        active = true;
        adsr.set_sample_rate(host_sample_rate);
        adsr.note_on();
        renderer.set_playback_rate(speed);
        renderer.start();
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
