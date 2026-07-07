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
#include <vector>

namespace pulp::signal {

template <typename SampleType = float>
class FreezeLoopSamplerT {
public:
    /// @param channels    channel count
    /// @param capacity    max samples per channel the record ring holds
    ///                    (must exceed the largest loop + crossfade + a block)
    /// @param crossfade   loop-boundary crossfade length in samples
    /// RT contract: prepare(), snapshot(), and restore() allocate or copy
    /// variable-size storage and are not audio-thread safe. After prepare(),
    /// write(), freeze(), release(), read(), reset(), and accessors are
    /// allocation-free for the prepared channel/capacity bounds.
    void prepare(int channels, int capacity, int crossfade) {
        channels_ = channels;
        capacity_ = std::max(capacity, crossfade + 2);
        crossfade_ = std::max(0, crossfade);
        ring_.assign(static_cast<size_t>(channels_) * capacity_, SampleType{0});
        loop_.assign(static_cast<size_t>(channels_) * capacity_, SampleType{0});
        write_pos_ = 0;
        written_ = 0;
        frozen_ = false;
        loop_len_ = 0;
        play_pos_ = 0;
    }

    int channels() const { return channels_; }
    bool frozen() const { return frozen_; }
    int loop_length() const { return loop_len_; }

    /// Record `n` frames into the ring (call every block, always).
    void write(const SampleType* const* in, int n) {
        for (int ch = 0; ch < channels_; ++ch) {
            SampleType* r = ring_.data() + static_cast<size_t>(ch) * capacity_;
            const SampleType* src = in[ch];
            int wp = write_pos_;
            for (int i = 0; i < n; ++i) {
                r[wp] = src[i];
                if (++wp >= capacity_) wp = 0;
            }
        }
        write_pos_ = (write_pos_ + n) % capacity_;
        written_ += n;
    }

    /// Freeze the most recent `loop_samples` of recorded audio into a
    /// seamless loop. Clamped to what the ring can supply.
    void freeze(int loop_samples) {
        const int avail = static_cast<int>(std::min<long long>(written_, capacity_));
        const int xf = std::min(crossfade_, std::max(0, avail - 1));
        int len = std::clamp(loop_samples, 1, std::max(1, avail - xf));
        loop_len_ = len;
        // The slice is the `len + xf` most-recent samples; index 0 is the
        // oldest of that window.
        const int span = len + xf;
        const int start = (write_pos_ - span + capacity_ * 2) % capacity_;
        for (int ch = 0; ch < channels_; ++ch) {
            const SampleType* r = ring_.data() + static_cast<size_t>(ch) * capacity_;
            SampleType* lp = loop_.data() + static_cast<size_t>(ch) * capacity_;
            for (int i = 0; i < len; ++i)
                lp[i] = r[(start + i) % capacity_];
            // Constant-power crossfade: blend the head with the tail that
            // follows the loop so end -> start is seamless.
            for (int i = 0; i < xf; ++i) {
                const SampleType t = (xf > 1)
                    ? static_cast<SampleType>(i) / static_cast<SampleType>(xf - 1)
                    : SampleType{1};
                const SampleType g_head = std::sin(static_cast<SampleType>(0.5)
                                                   * static_cast<SampleType>(3.14159265358979)
                                                   * t);
                const SampleType g_tail = std::cos(static_cast<SampleType>(0.5)
                                                   * static_cast<SampleType>(3.14159265358979)
                                                   * t);
                const SampleType head = lp[i];
                const SampleType tail = r[(start + len + i) % capacity_];
                lp[i] = head * g_head + tail * g_tail;
            }
        }
        play_pos_ = 0;
        frozen_ = true;
    }

    void release() { frozen_ = false; }

    /// Play the loop into `out` (frozen only). Does nothing if not frozen —
    /// the caller passes the live signal through in that case.
    void read(SampleType* const* out, int n) {
        if (!frozen_ || loop_len_ <= 0) return;
        for (int ch = 0; ch < channels_; ++ch) {
            const SampleType* lp = loop_.data() + static_cast<size_t>(ch) * capacity_;
            SampleType* dst = out[ch];
            int p = play_pos_;
            for (int i = 0; i < n; ++i) {
                dst[i] = lp[p];
                if (++p >= loop_len_) p = 0;
            }
        }
        play_pos_ = (play_pos_ + n) % loop_len_;
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), SampleType{0});
        write_pos_ = 0; written_ = 0; frozen_ = false; loop_len_ = 0; play_pos_ = 0;
    }

    // ── snapshot / restore (plugin state recall) ──
    /// Serialize the frozen loop: [channels][loop_len][crossfade][play_pos]
    /// then loop_len * channels samples. Empty when not frozen. Not RT-safe.
    std::vector<SampleType> snapshot() const {
        std::vector<SampleType> out;
        if (!frozen_ || loop_len_ <= 0) return out;
        out.reserve(static_cast<size_t>(4 + loop_len_ * channels_));
        out.push_back(static_cast<SampleType>(channels_));
        out.push_back(static_cast<SampleType>(loop_len_));
        out.push_back(static_cast<SampleType>(crossfade_));
        out.push_back(static_cast<SampleType>(play_pos_));
        for (int ch = 0; ch < channels_; ++ch) {
            const SampleType* lp = loop_.data() + static_cast<size_t>(ch) * capacity_;
            for (int i = 0; i < loop_len_; ++i) out.push_back(lp[i]);
        }
        return out;
    }

    /// Restore a loop produced by snapshot(). Returns false on a malformed
    /// or channel-mismatched blob (leaves the sampler unfrozen). Not RT-safe.
    bool restore(const std::vector<SampleType>& blob) {
        if (blob.size() < 4) { frozen_ = false; return false; }
        const int ch = static_cast<int>(blob[0]);
        const int len = static_cast<int>(blob[1]);
        const int pp = static_cast<int>(blob[3]);
        if (ch != channels_ || len <= 0 || len > capacity_) { frozen_ = false; return false; }
        if (blob.size() < static_cast<size_t>(4 + len * ch)) { frozen_ = false; return false; }
        loop_len_ = len;
        play_pos_ = (len > 0) ? (pp % len) : 0;
        size_t idx = 4;
        for (int c = 0; c < channels_; ++c) {
            SampleType* lp = loop_.data() + static_cast<size_t>(c) * capacity_;
            for (int i = 0; i < len; ++i) lp[i] = blob[idx++];
        }
        frozen_ = true;
        return true;
    }

private:
    int channels_ = 0;
    int capacity_ = 0;
    int crossfade_ = 0;
    std::vector<SampleType> ring_; // channels * capacity record ring
    std::vector<SampleType> loop_; // channels * capacity baked loop
    int write_pos_ = 0;
    long long written_ = 0;
    bool frozen_ = false;
    int loop_len_ = 0;
    int play_pos_ = 0;
};

using FreezeLoopSampler = FreezeLoopSamplerT<float>;
using FreezeLoopSampler64 = FreezeLoopSamplerT<double>;

} // namespace pulp::signal
