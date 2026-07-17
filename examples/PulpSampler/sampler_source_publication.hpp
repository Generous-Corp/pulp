#pragma once

#include "sampler_components.hpp"
#include "sampler_streaming_runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>

namespace pulp::examples {

class SamplerSourcePublicationOwner {
public:
    bool prepare() {
        release();
        return sample_store_.prepare() && mip_store_.prepare();
    }

    void release() noexcept {
        sample_store_.release();
        mip_store_.release();
        audio_sample_generation_.store(0, std::memory_order_relaxed);
        audio_selection_generation_.store(0, std::memory_order_relaxed);
    }

    bool load_mono(SamplerStreamingRuntime& streaming,
                   const float* data,
                   int frames,
                   float sample_rate) {
        if (!valid_load(data, frames, sample_rate)) return false;
        return publish(
            streaming,
            [&](std::uint64_t safe_generation) {
                return mip_store_.stage_mono(data, frames, sample_rate,
                                             safe_generation);
            },
            [&] {
                return sample_store_.load_mono(
                    data, frames, sample_rate,
                    audio_sample_generation_.load(std::memory_order_acquire));
            });
    }

    bool load_stereo(SamplerStreamingRuntime& streaming,
                     const float* interleaved,
                     int frames,
                     float sample_rate) {
        if (!valid_load(interleaved, frames, sample_rate)) return false;
        return publish(
            streaming,
            [&](std::uint64_t safe_generation) {
                return mip_store_.stage_interleaved_stereo(
                    interleaved, frames, sample_rate, safe_generation);
            },
            [&] {
                return sample_store_.load_interleaved_stereo(
                    interleaved, frames, sample_rate,
                    audio_sample_generation_.load(std::memory_order_acquire));
            });
    }

    SamplerSampleStore& sample_store() noexcept { return sample_store_; }
    const SamplerSampleStore& sample_store() const noexcept { return sample_store_; }

    template<std::size_t VoiceCount>
    void acknowledge_audio(const SamplerPublishedSource& published,
                           const SamplerVoice (&voices)[VoiceCount],
                           SamplerStreamingRuntime& streaming) noexcept {
        std::array<audio::PublishedSampleView, VoiceCount> active_views{};
        std::size_t active_count = 0;
        for (const auto& voice : voices) {
            if (voice.active && voice.sample.valid)
                active_views[active_count++] = voice.sample;
        }
        const auto acquired_resident =
            published.kind == SamplerPublishedSourceKind::Resident
            ? published.resident
            : audio::PublishedSampleView{};
        const auto sample_generation = audio::SampleSlotBank::oldest_active_generation(
            acquired_resident, active_views.data(), active_count);
        audio_sample_generation_.store(sample_generation, std::memory_order_release);

        std::uint64_t selection_generation = published.selection_generation;
        bool have_selection = published.kind != SamplerPublishedSourceKind::None;
        for (const auto& voice : voices) {
            if (!voice.active || voice.selection_generation == 0) continue;
            selection_generation = have_selection
                ? std::min(selection_generation, voice.selection_generation)
                : voice.selection_generation;
            have_selection = true;
        }
        const auto safe_selection = have_selection ? selection_generation : 0;
        streaming.acknowledge_selection(safe_selection);
        audio_selection_generation_.store(safe_selection,
                                          std::memory_order_release);
    }

private:
    SamplerSampleStore sample_store_;
    SamplerResidentMipStore mip_store_;
    std::atomic<std::uint64_t> audio_sample_generation_{0};
    std::atomic<std::uint64_t> audio_selection_generation_{0};
    std::mutex load_mutex_;

    bool valid_load(const float* data, int frames, float sample_rate) const noexcept {
        return data != nullptr && frames > 0 && sample_rate > 0.0f &&
               sample_store_.prepared() &&
               static_cast<std::uint64_t>(frames) <=
                   sample_store_.max_frames_per_slot();
    }

    template<typename MipStage, typename SampleLoad>
    bool publish(SamplerStreamingRuntime& streaming,
                 MipStage&& stage_mips,
                 SampleLoad&& load_sample) {
        std::lock_guard lock(load_mutex_);
        const bool mip_staged = stage_mips(
            audio_selection_generation_.load(std::memory_order_acquire));
        const bool loaded = streaming.load_and_publish_resident(
            std::forward<SampleLoad>(load_sample),
            [&] { return sample_store_.read_published_view(); },
            mip_staged ? mip_store_.staged_view() : SamplerMipPyramidView{},
            [&](std::uint64_t generation) {
                if (mip_staged) mip_store_.commit(generation);
                else mip_store_.commit_without_mips();
            });
        if (!loaded && mip_staged) mip_store_.discard_staged();
        return loaded;
    }
};

} // namespace pulp::examples
