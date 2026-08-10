#pragma once

/// @file freeze_loop_sampler.hpp
/// Capture-and-loop "freeze": snapshot a slice of recent audio and play it
/// back as a seamless loop.
///
/// Distinct from a spectral freeze (which holds one STFT frame forever):
/// this is a time-domain sampler. It records the incoming signal into a
/// ring continuously, so the instant freeze() is called the last
/// `loop_samples` of audio are already captured; that slice is then looped
/// with an equal-power crossfade across the loop boundary so the wrap is
/// click-free. Because it produces an ordinary audio stream, downstream
/// stages (pitch shift, formant, delay) keep working on the frozen loop —
/// freeze a moment, then bend it.
///
/// Crossfade scheme: the captured slice is `loop_samples` long plus a
/// `crossfade` tail of the samples that followed it. At bake time the head
/// `crossfade` samples are constant-power-blended with that tail (head
/// fading in, tail fading out), so playing the slice on repeat transitions
/// the end smoothly back into the start. Real-time-safe after prepare();
/// freeze()/read() allocate nothing.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

namespace pulp::signal {

template <typename SampleType = float> class FreezeLoopSamplerT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    enum class CaptureResult {
        Success,
        InvalidLength,
        InsufficientHistory,
        ExceedsCapacity,
    };

    /// @param channels    channel count
    /// @param capacity    max samples per channel the record ring holds
    ///                    (must exceed the largest loop + crossfade + a block)
    /// @param crossfade   loop-boundary crossfade length in samples
    /// RT contract: prepare(), snapshot(), and restore() allocate or copy
    /// variable-size storage and are not audio-thread safe. After prepare(),
    /// write(), freeze(), release(), read(), reset(), and accessors are
    /// allocation-free for the prepared channel/capacity bounds.
    void prepare(int channels, int capacity, int crossfade) {
        if (channels <= 0 || capacity <= 0 || crossfade < 0 ||
            crossfade > std::numeric_limits<int>::max() - 2) {
            fail_prepare();
            return;
        }
        const int bounded_capacity = std::max(capacity, crossfade + 2);
        if constexpr (std::numeric_limits<SampleType>::digits < std::numeric_limits<int>::digits) {
            constexpr auto exact_integer_limit = std::uint64_t{1}
                                                 << std::numeric_limits<SampleType>::digits;
            if (static_cast<std::uint64_t>(bounded_capacity) > exact_integer_limit ||
                static_cast<std::uint64_t>(channels) > exact_integer_limit) {
                fail_prepare();
                return;
            }
        }
        const auto channel_count = static_cast<std::size_t>(channels);
        const auto frame_capacity = static_cast<std::size_t>(bounded_capacity);
        if (channel_count > std::numeric_limits<std::size_t>::max() / frame_capacity) {
            fail_prepare();
            return;
        }
        const auto elements = channel_count * frame_capacity;
        if (elements > ring_.max_size() || elements > loop_.max_size()) {
            fail_prepare();
            return;
        }

        std::vector<SampleType> replacement_ring(elements, SampleType{});
        std::vector<SampleType> replacement_loop(elements, SampleType{});
        ring_.swap(replacement_ring);
        loop_.swap(replacement_loop);
        channels_ = channels;
        capacity_ = bounded_capacity;
        crossfade_ = crossfade;
        write_pos_ = 0;
        written_ = 0;
        frozen_ = false;
        loop_len_ = 0;
        loop_start_ = 0;
        play_pos_ = 0;
    }

    int channels() const noexcept {
        return channels_;
    }
    int capacity() const noexcept {
        return capacity_;
    }
    int available_history() const noexcept {
        return static_cast<int>(std::min<long long>(written_, capacity_));
    }
    bool frozen() const noexcept {
        return frozen_;
    }
    int loop_length() const noexcept {
        return loop_len_;
    }
    std::size_t retained_bytes() const noexcept {
        return (ring_.capacity() + loop_.capacity()) * sizeof(SampleType);
    }

    /// Record `n` frames into the ring (call every block, always).
    void write(const SampleType* const* in, int n) {
        if (n <= 0 || channels_ <= 0 || capacity_ <= 0 || in == nullptr)
            return;
        for (int ch = 0; ch < channels_; ++ch) {
            if (in[ch] == nullptr)
                return;
        }
        for (int ch = 0; ch < channels_; ++ch) {
            SampleType* r = ring_.data() + static_cast<size_t>(ch) * capacity_;
            const SampleType* src = in[ch];
            int wp = write_pos_;
            for (int i = 0; i < n; ++i) {
                r[wp] = std::isfinite(static_cast<double>(src[i])) ? src[i] : SampleType{};
                if (++wp >= capacity_)
                    wp = 0;
            }
        }
        write_pos_ = static_cast<int>((static_cast<std::int64_t>(write_pos_) + n) % capacity_);
        written_ = std::min<long long>(capacity_, written_ + std::min(n, capacity_));
    }

    /// Capture exactly the most recent `loop_samples` frames. Unlike freeze(),
    /// this never clamps or pads: callers that own musical timing can reject a
    /// gesture without changing the current loop. The prepared ring and loop
    /// buffers exchange roles in constant time; the captured region remains
    /// circular and the former loop storage becomes an empty rolling history.
    /// No transition is baked into the captured region.
    CaptureResult try_freeze_recent(int loop_samples) noexcept {
        if (loop_samples <= 0)
            return CaptureResult::InvalidLength;
        if (loop_samples > capacity_)
            return CaptureResult::ExceedsCapacity;
        if (loop_samples > available_history())
            return CaptureResult::InsufficientHistory;

        const int start = static_cast<int>(
            (static_cast<std::int64_t>(write_pos_) - loop_samples + capacity_) % capacity_);
        ring_.swap(loop_);
        loop_len_ = loop_samples;
        loop_start_ = start;
        play_pos_ = 0;
        frozen_ = true;
        clear_history();
        return CaptureResult::Success;
    }

    SampleType loop_sample(int channel, int frame) const noexcept {
        if (!frozen_ || channel < 0 || channel >= channels_ || frame < 0 || frame >= loop_len_)
            return SampleType{};
        auto loop_index = static_cast<std::size_t>(loop_start_) + static_cast<std::size_t>(frame);
        if (loop_index >= static_cast<std::size_t>(capacity_))
            loop_index -= static_cast<std::size_t>(capacity_);
        return loop_[static_cast<std::size_t>(channel) * capacity_ + loop_index];
    }

    /// Freeze the most recent `loop_samples` of recorded audio into a
    /// seamless loop. Clamped to what the ring can supply.
    void freeze(int loop_samples) {
        const int avail = static_cast<int>(std::min<long long>(written_, capacity_));
        if (avail <= 0) {
            frozen_ = false;
            loop_len_ = 0;
            loop_start_ = 0;
            play_pos_ = 0;
            return;
        }
        const int xf = std::min(crossfade_, std::max(0, avail - 1));
        int len = std::clamp(loop_samples, 1, std::max(1, avail - xf));
        loop_len_ = len;
        // The slice is the `len + xf` most-recent samples; index 0 is the
        // oldest of that window.
        const int span = len + xf;
        const auto capacity = static_cast<std::size_t>(capacity_);
        const auto start =
            (static_cast<std::size_t>(write_pos_) + capacity - static_cast<std::size_t>(span)) %
            capacity;
        for (int ch = 0; ch < channels_; ++ch) {
            const SampleType* r = ring_.data() + static_cast<size_t>(ch) * capacity_;
            SampleType* lp = loop_.data() + static_cast<size_t>(ch) * capacity_;
            for (int i = 0; i < len; ++i)
                lp[i] = r[(start + static_cast<std::size_t>(i)) % capacity];
            // Constant-power crossfade: blend the head with the tail that
            // follows the loop so end -> start is seamless.
            for (int i = 0; i < xf; ++i) {
                const SampleType t =
                    (xf > 1) ? static_cast<SampleType>(i) / static_cast<SampleType>(xf - 1)
                             : SampleType{1};
                const SampleType g_head = std::sin(static_cast<SampleType>(0.5) *
                                                   static_cast<SampleType>(3.14159265358979) * t);
                const SampleType g_tail = std::cos(static_cast<SampleType>(0.5) *
                                                   static_cast<SampleType>(3.14159265358979) * t);
                const SampleType head = lp[i];
                const SampleType tail =
                    r[(start + static_cast<std::size_t>(len) + static_cast<std::size_t>(i)) %
                      capacity];
                lp[i] = head * g_head + tail * g_tail;
            }
        }
        loop_start_ = 0;
        play_pos_ = 0;
        frozen_ = true;
    }

    void release() {
        frozen_ = false;
    }

    /// Play the loop into `out` (frozen only). Does nothing if not frozen —
    /// the caller passes the live signal through in that case.
    void read(SampleType* const* out, int n) {
        if (!frozen_ || loop_len_ <= 0 || n <= 0 || out == nullptr)
            return;
        for (int ch = 0; ch < channels_; ++ch) {
            if (out[ch] == nullptr)
                return;
        }
        for (int ch = 0; ch < channels_; ++ch) {
            SampleType* dst = out[ch];
            int p = play_pos_;
            for (int i = 0; i < n; ++i) {
                dst[i] = loop_sample(ch, p);
                if (++p >= loop_len_)
                    p = 0;
            }
        }
        play_pos_ = static_cast<int>(
            (static_cast<std::uint64_t>(play_pos_) + static_cast<std::uint64_t>(n)) %
            static_cast<std::uint64_t>(loop_len_));
    }

    void reset() {
        clear_history();
        frozen_ = false;
        loop_len_ = 0;
        loop_start_ = 0;
        play_pos_ = 0;
    }

    /// Invalidate rolling input continuity without changing a captured loop.
    /// State recall uses this after restoring the persistent loop because the
    /// pre-recall ring belongs to a different transport history.
    void clear_history() noexcept {
        write_pos_ = 0;
        written_ = 0;
    }

    // ── snapshot / restore (plugin state recall) ──
    /// Serialize the frozen loop: [channels][loop_len][crossfade][play_pos]
    /// then loop_len * channels samples. Empty when not frozen. Not RT-safe.
    std::vector<SampleType> snapshot() const {
        std::vector<SampleType> out;
        if (!frozen_ || loop_len_ <= 0)
            return out;
        const auto payload =
            static_cast<std::size_t>(loop_len_) * static_cast<std::size_t>(channels_);
        if (payload > out.max_size() - 4)
            return {};
        out.reserve(4 + payload);
        out.push_back(static_cast<SampleType>(channels_));
        out.push_back(static_cast<SampleType>(loop_len_));
        out.push_back(static_cast<SampleType>(crossfade_));
        out.push_back(static_cast<SampleType>(play_pos_));
        for (int ch = 0; ch < channels_; ++ch) {
            for (int i = 0; i < loop_len_; ++i)
                out.push_back(loop_sample(ch, i));
        }
        return out;
    }

    /// Restore a loop produced by snapshot(). Returns false on a malformed or
    /// channel-mismatched blob without changing the current loop. Not RT-safe.
    bool restore(const std::vector<SampleType>& blob) {
        if (blob.size() < 4)
            return false;
        for (const auto value : blob) {
            if (!std::isfinite(static_cast<double>(value)))
                return false;
        }
        const auto valid_integer = [](SampleType value, SampleType minimum,
                                      SampleType maximum) noexcept {
            return value >= minimum && value <= maximum && std::trunc(value) == value;
        };
        if (!valid_integer(blob[0], SampleType{1}, static_cast<SampleType>(channels_)) ||
            !valid_integer(blob[1], SampleType{1}, static_cast<SampleType>(capacity_)) ||
            !valid_integer(blob[2], SampleType{}, static_cast<SampleType>(capacity_)))
            return false;
        const int ch = static_cast<int>(blob[0]);
        const int len = static_cast<int>(blob[1]);
        if (ch != channels_ ||
            !valid_integer(blob[3], SampleType{}, static_cast<SampleType>(len - 1)))
            return false;
        const auto expected_size = static_cast<std::size_t>(4) +
                                   static_cast<std::size_t>(len) * static_cast<std::size_t>(ch);
        if (blob.size() != expected_size)
            return false;
        const int pp = static_cast<int>(blob[3]);
        loop_len_ = len;
        loop_start_ = 0;
        play_pos_ = pp;
        size_t idx = 4;
        for (int c = 0; c < channels_; ++c) {
            SampleType* lp = loop_.data() + static_cast<size_t>(c) * capacity_;
            for (int i = 0; i < len; ++i)
                lp[i] = blob[idx++];
        }
        frozen_ = true;
        return true;
    }

  private:
    void fail_prepare() noexcept {
        channels_ = 0;
        capacity_ = 0;
        crossfade_ = 0;
        ring_.clear();
        loop_.clear();
        write_pos_ = 0;
        written_ = 0;
        frozen_ = false;
        loop_len_ = 0;
        loop_start_ = 0;
        play_pos_ = 0;
    }

    int channels_ = 0;
    int capacity_ = 0;
    int crossfade_ = 0;
    std::vector<SampleType> ring_; // channels * capacity record ring
    std::vector<SampleType> loop_; // channels * capacity baked loop
    int write_pos_ = 0;
    long long written_ = 0;
    bool frozen_ = false;
    int loop_len_ = 0;
    int loop_start_ = 0;
    int play_pos_ = 0;
};

using FreezeLoopSampler = FreezeLoopSamplerT<float>;
using FreezeLoopSampler64 = FreezeLoopSamplerT<double>;

} // namespace pulp::signal
