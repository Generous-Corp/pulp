#pragma once

/// @file reverse_buffer.hpp
/// Fixed-capacity streaming window reversal.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulp::signal {

enum class ReverseBufferState { unprepared, bypassed, priming, running };

/// A mono streaming reverse-window processor.
///
/// One prepared buffer is captured forward while the preceding buffer is read
/// backward. The first reversed window begins after exactly `window_samples`
/// input frames. This is a reorder effect: individual sample delays span one
/// through `2 * window_samples - 1`, even though the declared startup latency
/// is one complete window. Instantiate one processor per independently reversed
/// channel.
template <typename SampleType = float> class ReverseBufferT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);

    static constexpr std::size_t kMinimumWindowSamples = 2;
    static constexpr std::size_t kMaximumWindowSamples = 8u * 1024u * 1024u;

    struct Config {
        std::size_t window_samples = 1024;
        /// Zero applies an exact rectangular window. Values of two or greater
        /// apply a raised-cosine fade to zero at both playback boundaries.
        std::size_t boundary_fade_samples = 0;
    };

    [[nodiscard]] bool prepare(std::size_t maximum_window_samples) {
        if (!valid_maximum(maximum_window_samples) ||
            config_.window_samples > maximum_window_samples)
            return false;

        std::array<std::vector<SampleType>, 2> replacement;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
#endif
            replacement[0].assign(maximum_window_samples, SampleType{});
            replacement[1].assign(maximum_window_samples, SampleType{});
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }
#endif

        buffers_ = std::move(replacement);
        maximum_window_samples_ = maximum_window_samples;
        prepared_ = true;
        discard_history();
        return true;
    }

    /// Publishes the complete transport configuration. Accepted changes discard
    /// captured audio and restart priming; rejection preserves all state.
    [[nodiscard]] bool configure(Config replacement) noexcept {
        if (!valid_config(replacement) ||
            (prepared_ && replacement.window_samples > maximum_window_samples_))
            return false;
        config_ = replacement;
        discard_history();
        return true;
    }

    Config config() const noexcept {
        return config_;
    }
    bool prepared() const noexcept {
        return prepared_;
    }
    std::size_t maximum_window_samples() const noexcept {
        return maximum_window_samples_;
    }
    std::size_t retained_bytes() const noexcept {
        return prepared_ ? 2u * maximum_window_samples_ * sizeof(SampleType) : 0u;
    }

    int latency_samples() const noexcept {
        return prepared_ && !bypassed_ ? static_cast<int>(config_.window_samples) : 0;
    }
    int startup_latency_samples() const noexcept {
        return prepared_ ? static_cast<int>(config_.window_samples) : 0;
    }
    int tail_samples() const noexcept {
        if (!prepared_ || bypassed_)
            return 0;
        return static_cast<int>(2u * config_.window_samples - 1u);
    }

    ReverseBufferState state() const noexcept {
        if (!prepared_)
            return ReverseBufferState::unprepared;
        if (bypassed_)
            return ReverseBufferState::bypassed;
        return playback_ready_ ? ReverseBufferState::running : ReverseBufferState::priming;
    }

    std::size_t position() const noexcept {
        return position_;
    }

    /// Hard transport bypass. Finite input passes through sample-exactly and no
    /// audio is captured. Either transition discards history, so re-engaging the
    /// effect always starts with a fresh complete priming window.
    void set_bypassed(bool bypassed) noexcept {
        if (bypassed_ == bypassed)
            return;
        bypassed_ = bypassed;
        discard_history();
    }
    bool bypassed() const noexcept {
        return bypassed_;
    }

    /// Discards capture/playback state in constant time without changing the
    /// prepared capacity, configuration, or bypass state. Hosts should call
    /// this after a transport seek or any discontinuous source reposition.
    void discard_history() noexcept {
        capture_buffer_ = 0;
        playback_buffer_ = 1;
        position_ = 0;
        playback_ready_ = false;
    }

    void reset() noexcept {
        discard_history();
    }

    SampleType process_sample(SampleType input) noexcept {
        if (!std::isfinite(static_cast<double>(input))) {
            discard_history();
            return SampleType{};
        }
        if (bypassed_)
            return input;
        if (!prepared_)
            return SampleType{};

        buffers_[capture_buffer_][position_] = input;
        SampleType output{};
        if (playback_ready_) {
            const auto reverse_position = config_.window_samples - 1u - position_;
            const auto gain = static_cast<SampleType>(boundary_gain(position_));
            output = buffers_[playback_buffer_][reverse_position] * gain;
            if (!std::isfinite(static_cast<double>(output))) {
                discard_history();
                return SampleType{};
            }
        }

        ++position_;
        if (position_ == config_.window_samples) {
            playback_buffer_ = capture_buffer_;
            capture_buffer_ = 1u - capture_buffer_;
            position_ = 0;
            playback_ready_ = true;
        }
        return output;
    }

    void process_block(const SampleType* input, SampleType* output,
                       std::size_t sample_count) noexcept {
        if (input == nullptr || output == nullptr)
            return;
        for (std::size_t i = 0; i < sample_count; ++i) {
            const auto sample = input[i];
            output[i] = process_sample(sample);
        }
    }

  private:
    static bool valid_maximum(std::size_t samples) noexcept {
        return samples >= kMinimumWindowSamples && samples <= kMaximumWindowSamples;
    }

    static bool valid_config(Config config) noexcept {
        if (!valid_maximum(config.window_samples) ||
            config.boundary_fade_samples > config.window_samples / 2u)
            return false;
        return config.boundary_fade_samples == 0u || config.boundary_fade_samples >= 2u;
    }

    double boundary_gain(std::size_t playback_position) const noexcept {
        const auto fade = config_.boundary_fade_samples;
        if (fade == 0u)
            return 1.0;
        const auto from_end = config_.window_samples - 1u - playback_position;
        const auto nearest_boundary = std::min(playback_position, from_end);
        if (nearest_boundary >= fade)
            return 1.0;
        const double phase = static_cast<double>(nearest_boundary) / static_cast<double>(fade);
        constexpr double kPi = 3.14159265358979323846264338327950288;
        return 0.5 * (1.0 - std::cos(kPi * phase));
    }

    Config config_{};
    std::array<std::vector<SampleType>, 2> buffers_{};
    std::size_t maximum_window_samples_ = 0;
    std::size_t capture_buffer_ = 0;
    std::size_t playback_buffer_ = 1;
    std::size_t position_ = 0;
    bool playback_ready_ = false;
    bool prepared_ = false;
    bool bypassed_ = false;
};

using ReverseBuffer = ReverseBufferT<float>;
using ReverseBuffer64 = ReverseBufferT<double>;

} // namespace pulp::signal
