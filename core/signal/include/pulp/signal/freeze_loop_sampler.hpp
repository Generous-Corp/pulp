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
#include <span>
#include <type_traits>
#include <vector>

namespace pulp::signal {

template <typename SampleType = float> class FreezeLoopSamplerT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    /// @param channels    channel count
    /// @param capacity    max samples per channel the record ring holds
    ///                    (must exceed the largest loop + crossfade + a block)
    /// @param crossfade   loop-boundary crossfade length in samples
    /// Invalid or unrepresentable bounds fail closed: channels() and
    /// capacity() become zero and processing calls are no-ops.
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

        channels_ = channels;
        capacity_ = bounded_capacity;
        crossfade_ = crossfade;
        ring_.assign(elements, SampleType{0});
        loop_.assign(elements, SampleType{0});
        write_pos_ = 0;
        written_ = 0;
        frozen_ = false;
        loop_len_ = 0;
        play_pos_ = 0;
    }

    int channels() const noexcept {
        return channels_;
    }
    int capacity() const noexcept {
        return capacity_;
    }
    int available_history() const noexcept {
        return static_cast<int>(
            std::min<std::uint64_t>(written_, static_cast<std::uint64_t>(capacity_)));
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
            SampleType* r = ring_.data() + static_cast<size_t>(ch) * capacity_;
            const SampleType* src = in[ch];
            int wp = write_pos_;
            for (int i = 0; i < n; ++i) {
                r[wp] = src[i];
                if (++wp >= capacity_)
                    wp = 0;
            }
        }
        write_pos_ = static_cast<int>(
            (static_cast<std::uint64_t>(write_pos_) + static_cast<std::uint64_t>(n)) %
            static_cast<std::uint64_t>(capacity_));
        const auto frames = static_cast<std::uint64_t>(n);
        written_ = frames > std::numeric_limits<std::uint64_t>::max() - written_
                       ? std::numeric_limits<std::uint64_t>::max()
                       : written_ + frames;
    }

    /// Record one planar frame without constructing temporary channel arrays.
    /// `frame[ch]` is copied into channel `ch`; invalid input is ignored.
    void write_frame(const SampleType* frame) noexcept {
        if (frame == nullptr || channels_ <= 0 || capacity_ <= 0)
            return;
        for (int ch = 0; ch < channels_; ++ch)
            ring_[static_cast<std::size_t>(ch) * static_cast<std::size_t>(capacity_) +
                  static_cast<std::size_t>(write_pos_)] = frame[ch];
        if (++write_pos_ >= capacity_)
            write_pos_ = 0;
        if (written_ != std::numeric_limits<std::uint64_t>::max())
            ++written_;
    }

    /// Copy exactly the most recent `frames` into the immutable capture.
    /// Unlike legacy freeze(), this never clamps or bakes a boundary crossfade:
    /// callers that own rhythmic playback need the unmodified half-open source
    /// interval [now - frames, now). A failed request preserves an existing
    /// capture, which makes active retrigger rejection atomic.
    [[nodiscard]] bool capture_recent_exact(int frames) noexcept {
        if (frames <= 0 || frames > capacity_ || frames > available_history())
            return false;
        const auto capacity = static_cast<std::size_t>(capacity_);
        const auto start =
            (static_cast<std::size_t>(write_pos_) + capacity - static_cast<std::size_t>(frames)) %
            capacity;
        for (int ch = 0; ch < channels_; ++ch) {
            const SampleType* source =
                ring_.data() + static_cast<std::size_t>(ch) * static_cast<std::size_t>(capacity_);
            SampleType* destination =
                loop_.data() + static_cast<std::size_t>(ch) * static_cast<std::size_t>(capacity_);
            for (int i = 0; i < frames; ++i)
                destination[i] = source[(start + static_cast<std::size_t>(i)) % capacity];
        }
        loop_len_ = frames;
        play_pos_ = 0;
        frozen_ = true;
        return true;
    }

    /// Immutable contiguous access to the current exact/frozen capture.
    [[nodiscard]] std::span<const SampleType> captured_channel(int channel) const noexcept {
        if (!frozen_ || channel < 0 || channel >= channels_ || loop_len_ <= 0)
            return {};
        return {loop_.data() +
                    static_cast<std::size_t>(channel) * static_cast<std::size_t>(capacity_),
                static_cast<std::size_t>(loop_len_)};
    }

    /// Freeze the most recent `loop_samples` of recorded audio into a
    /// seamless loop. Clamped to what the ring can supply.
    void freeze(int loop_samples) {
        const int avail = available_history();
        if (channels_ <= 0 || capacity_ <= 0 || avail <= 0) {
            frozen_ = false;
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
            const SampleType* lp = loop_.data() + static_cast<size_t>(ch) * capacity_;
            SampleType* dst = out[ch];
            int p = play_pos_;
            for (int i = 0; i < n; ++i) {
                dst[i] = lp[p];
                if (++p >= loop_len_)
                    p = 0;
            }
        }
        play_pos_ = static_cast<int>(
            (static_cast<std::uint64_t>(play_pos_) + static_cast<std::uint64_t>(n)) %
            static_cast<std::uint64_t>(loop_len_));
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), SampleType{0});
        write_pos_ = 0;
        written_ = 0;
        frozen_ = false;
        loop_len_ = 0;
        play_pos_ = 0;
    }

    /// Start a new dry-history epoch without touching an immutable capture.
    /// Old ring bytes become unreachable immediately and are overwritten by
    /// subsequent writes; this is O(1) and safe on the audio thread.
    void invalidate_history() noexcept {
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
        out.reserve(static_cast<std::size_t>(4) + payload);
        out.push_back(static_cast<SampleType>(channels_));
        out.push_back(static_cast<SampleType>(loop_len_));
        out.push_back(static_cast<SampleType>(crossfade_));
        out.push_back(static_cast<SampleType>(play_pos_));
        for (int ch = 0; ch < channels_; ++ch) {
            const SampleType* lp = loop_.data() + static_cast<size_t>(ch) * capacity_;
            for (int i = 0; i < loop_len_; ++i)
                out.push_back(lp[i]);
        }
        return out;
    }

    /// Restore a loop produced by snapshot(). Returns false on a malformed
    /// or channel-mismatched blob (leaves the sampler unfrozen). Not RT-safe.
    bool restore(const std::vector<SampleType>& blob) {
        if (blob.size() < 4) {
            frozen_ = false;
            return false;
        }
        for (std::size_t i = 0; i < 4; ++i) {
            if (!std::isfinite(blob[i])) {
                frozen_ = false;
                return false;
            }
        }
        const auto maximum_int = static_cast<long double>(std::numeric_limits<int>::max());
        const auto minimum_int = static_cast<long double>(std::numeric_limits<int>::min());
        for (std::size_t i = 0; i < 4; ++i) {
            const auto value = static_cast<long double>(blob[i]);
            if (value < minimum_int || value > maximum_int || std::trunc(value) != value) {
                frozen_ = false;
                return false;
            }
        }
        const int ch = static_cast<int>(blob[0]);
        const int len = static_cast<int>(blob[1]);
        const int xf = static_cast<int>(blob[2]);
        const int pp = static_cast<int>(blob[3]);
        if (ch != channels_ || len <= 0 || len > capacity_ || xf < 0 || xf > capacity_ || pp < 0 ||
            pp >= len) {
            frozen_ = false;
            return false;
        }
        const auto channels = static_cast<std::size_t>(ch);
        const auto length = static_cast<std::size_t>(len);
        if (channels != 0 && length > (std::numeric_limits<std::size_t>::max() - 4u) / channels) {
            frozen_ = false;
            return false;
        }
        const auto required = 4u + length * channels;
        if (blob.size() != required) {
            frozen_ = false;
            return false;
        }
        for (std::size_t i = 4; i < required; ++i) {
            if (!std::isfinite(blob[i])) {
                frozen_ = false;
                return false;
            }
        }
        loop_len_ = len;
        play_pos_ = (len > 0) ? (pp % len) : 0;
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
        play_pos_ = 0;
    }

    int channels_ = 0;
    int capacity_ = 0;
    int crossfade_ = 0;
    std::vector<SampleType> ring_; // channels * capacity record ring
    std::vector<SampleType> loop_; // channels * capacity baked loop
    int write_pos_ = 0;
    std::uint64_t written_ = 0;
    bool frozen_ = false;
    int loop_len_ = 0;
    int play_pos_ = 0;
};

using FreezeLoopSampler = FreezeLoopSamplerT<float>;
using FreezeLoopSampler64 = FreezeLoopSamplerT<double>;

} // namespace pulp::signal
