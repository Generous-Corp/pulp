#pragma once

#include <pulp/audio/sample_mip_builder.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

namespace pulp::examples {

struct SamplerMipLevelView {
    std::array<const float*, 2> channels{};
    std::uint64_t frames = 0;
    double sample_rate = 0.0;
    std::uint32_t octave = 0;

    bool valid() const noexcept {
        return octave > 0 && frames > 0 && sample_rate > 0.0 && channels[0] != nullptr;
    }
};

struct SamplerMipPyramidView {
    static constexpr std::size_t kMaximumLevels = 16;

    std::array<SamplerMipLevelView, kMaximumLevels> levels{};
    std::uint32_t level_count = 0;

    const SamplerMipLevelView* level(std::uint32_t octave) const& noexcept {
        if (octave == 0 || octave > level_count)
            return nullptr;
        const auto& candidate = levels[octave - 1];
        return candidate.valid() && candidate.octave == octave ? &candidate : nullptr;
    }
    const SamplerMipLevelView* level(std::uint32_t) const&& = delete;
};

static_assert(std::is_trivially_copyable_v<SamplerMipPyramidView>);

class SamplerResidentMipStore {

  public:
    static constexpr std::uint64_t kDefaultByteBudget = 256ull * 1024ull * 1024ull;
    // Larger resident assets still publish, but omit mips so synchronous control
    // calls cannot turn a multi-minute sample into billions of FIR operations.
    static constexpr std::uint64_t kMaximumBuildSamples = 48000ull * 2ull;

    bool prepare() {
        release();
        coefficients_ = audio::design_sample_mip_decimator();
        return !coefficients_.empty() && (coefficients_.size() & 1u) != 0u;
    }

    bool prepared() const noexcept { return !coefficients_.empty(); }

    void release() noexcept {
        coefficients_.clear();
        for (auto& slot : slots_)
            slot = {};
        current_slot_ = kNoSlot;
        staged_slot_ = kNoSlot;
    }

    bool stage_mono(const float* source, std::uint64_t frames, double sample_rate,
                    std::uint64_t audio_safe_selection) {
        const float* channels[] = {source};
        return stage(channels, 1, frames, sample_rate, audio_safe_selection);
    }

    bool stage_interleaved_stereo(const float* source, std::uint64_t frames, double sample_rate,
                                  std::uint64_t audio_safe_selection) {
        if (source == nullptr || frames < coefficients_.size() * 2 ||
            frames > kMaximumBuildSamples / 2)
            return false;
        deinterleave_[0].resize(static_cast<std::size_t>(frames));
        deinterleave_[1].resize(static_cast<std::size_t>(frames));
        for (std::uint64_t frame = 0; frame < frames; ++frame) {
            deinterleave_[0][static_cast<std::size_t>(frame)] =
                source[static_cast<std::size_t>(frame * 2)];
            deinterleave_[1][static_cast<std::size_t>(frame)] =
                source[static_cast<std::size_t>(frame * 2 + 1)];
        }
        const float* channels[] = {deinterleave_[0].data(), deinterleave_[1].data()};
        const bool built = stage(channels, 2, frames, sample_rate, audio_safe_selection);
        deinterleave_[0].clear();
        deinterleave_[1].clear();
        return built;
    }

    void commit(std::uint64_t selection_generation) noexcept {
        if (staged_slot_ == kNoSlot || selection_generation == 0)
            return;
        auto& slot = slots_[staged_slot_];
        slot.selection_generation = selection_generation;
        current_slot_ = staged_slot_;
        staged_slot_ = kNoSlot;
    }

    void commit_without_mips() noexcept {
        current_slot_ = kNoSlot;
    }

    void discard_staged() noexcept {
        if (staged_slot_ == kNoSlot)
            return;
        slots_[staged_slot_] = {};
        staged_slot_ = kNoSlot;
    }

    SamplerMipPyramidView staged_view() const noexcept {
        return staged_slot_ == kNoSlot ? SamplerMipPyramidView{} : slots_[staged_slot_].view;
    }

  private:
    struct LevelStorage {
        std::array<std::vector<float>, 2> channels;
    };

    struct Slot {
        std::array<LevelStorage, SamplerMipPyramidView::kMaximumLevels> levels;
        SamplerMipPyramidView view{};
        std::uint64_t selection_generation = 0;
    };

    static constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();

    std::array<Slot, 2> slots_{};
    std::array<std::vector<float>, 2> deinterleave_{};
    std::vector<double> coefficients_;
    std::size_t current_slot_ = kNoSlot;
    std::size_t staged_slot_ = kNoSlot;

    std::size_t reusable_slot(std::uint64_t audio_safe_selection) const noexcept {
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            if (index == current_slot_ || index == staged_slot_)
                continue;
            const auto generation = slots_[index].selection_generation;
            if (generation == 0 ||
                (audio_safe_selection != 0 && generation < audio_safe_selection)) {
                return index;
            }
        }
        return kNoSlot;
    }

    bool stage(const float* const* source, std::uint32_t channels, std::uint64_t frames,
               double sample_rate, std::uint64_t audio_safe_selection) {
        if (source == nullptr || channels == 0 || channels > 2 ||
            frames < coefficients_.size() * 2 || frames > kMaximumBuildSamples / channels ||
            !(sample_rate > 0.0) || coefficients_.empty()) {
            return false;
        }
        const auto slot_index = reusable_slot(audio_safe_selection);
        if (slot_index == kNoSlot)
            return false;

        auto& slot = slots_[slot_index];
        slot = {};
        std::uint64_t level_frames = frames;
        std::uint64_t total_samples = 0;
        std::uint32_t level_count = 0;
        while (level_frames >= coefficients_.size() * 2 &&
               level_count < SamplerMipPyramidView::kMaximumLevels) {
            level_frames = (level_frames + 1) / 2;
            if (level_frames > (kDefaultByteBudget / sizeof(float) - total_samples) / channels) {
                break;
            }
            total_samples += level_frames * channels;
            ++level_count;
        }
        if (level_count == 0)
            return false;

        const float* input_channels[2] = {source[0], channels > 1 ? source[1] : nullptr};
        std::uint64_t input_frames = frames;
        for (std::uint32_t level = 0; level < level_count; ++level) {
            auto& storage = slot.levels[level];
            const auto output_frames = (input_frames + 1) / 2;
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                storage.channels[channel].resize(static_cast<std::size_t>(output_frames));
                audio::decimate_sample_mip_2x(input_channels[channel], input_frames,

                                              storage.channels[channel].data(), output_frames,
                                              coefficients_);
                slot.view.levels[level].channels[channel] = storage.channels[channel].data();
            }
            slot.view.levels[level].frames = output_frames;
            slot.view.levels[level].sample_rate =
                std::ldexp(sample_rate, -static_cast<int>(level + 1));
            slot.view.levels[level].octave = level + 1;
            input_frames = output_frames;
            for (std::uint32_t channel = 0; channel < channels; ++channel)
                input_channels[channel] = storage.channels[channel].data();
        }
        slot.view.level_count = level_count;
        staged_slot_ = slot_index;
        return true;
    }
};

// Octave mips are lossless coordinate changes only at exact powers of two.
// Fractional ratios use the ratio-tracking sinc path instead of stepping to a
// prematurely narrow mip level.
inline std::uint32_t sampler_exact_mip_octave(double source_frames_per_output) noexcept {
    if (!(source_frames_per_output > 1.0) || !std::isfinite(source_frames_per_output))
        return 0;
    const auto octave = std::round(std::log2(source_frames_per_output));
    if (!(octave > 0.0) || octave > 32.0)
        return 0;
    const auto exact_rate = std::ldexp(1.0, static_cast<int>(octave));
    const auto tolerance = std::max(1.0, exact_rate) * 1e-9;
    return std::abs(source_frames_per_output - exact_rate) <= tolerance
               ? static_cast<std::uint32_t>(octave)

               : 0;
}

} // namespace pulp::examples
