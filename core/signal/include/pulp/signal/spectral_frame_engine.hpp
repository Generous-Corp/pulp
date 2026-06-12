#pragma once

/// @file spectral_frame_engine.hpp
/// Streaming STFT analysis + weighted overlap-add (WOLA) synthesis for
/// DSP-grade spectral processing — distinct from the visualization-only
/// `Stft`, which has no synthesis path.
///
/// Channels are processed as a coherent group: every analysis callback
/// delivers all channels' spectra for the same time index, so spectral
/// modifications (pitch/time, formant, freeze) can make one decision per
/// frame and apply it to the whole group.
///
/// Two usage levels:
///   - `process()` — equal-hop analysis→modify→resynthesis streaming with a
///     per-frame callback. Neutral (identity callback) reconstruction is
///     exact up to float rounding for any COLA-satisfying window/hop.
///   - `analyze()` / `synthesize_frame()` / `read_output()` — split API for
///     processors that need a synthesis hop different from the analysis hop
///     (time-scale modification). Output availability is tracked so callers
///     can pull exactly what is final.
///
/// Reconstruction is normalized per-sample by the accumulated squared
/// synthesis window, which keeps amplitude exact for any hop (including
/// variable hops and stream edges) without hardcoded COLA constants.
///
/// Basis: Allen & Rabiner 1977 (unified STFT analysis/synthesis);
/// Crochiere 1980 (weighted overlap-add). No allocation or locks after
/// `prepare()`.

#include <pulp/signal/fft.hpp>
#include <pulp/signal/windowing.hpp>
#include <algorithm>
#include <cassert>
#include <complex>
#include <cstdint>
#include <vector>

namespace pulp::signal {

/// Configuration for a SpectralFrameEngine.
struct SpectralFrameEngineConfig {
    int fft_size = 2048;       // Power of 2, 256–16384
    int analysis_hop = 512;    // Samples between analysis frames (<= fft_size/2)
    int channels = 1;          // Channel-group size (>= 1)
    int max_block = 4096;      // Largest process() block the caller will use
    int max_synthesis_hop = 0; // 0 = 2 * analysis_hop; upper bound for synthesize_frame()
    WindowFunction::Type window = WindowFunction::Type::hann;
};

/// Streaming multichannel STFT/WOLA engine. Not thread-safe; one instance
/// per audio stream.
class SpectralFrameEngine {
public:
    SpectralFrameEngine() = default;

    /// RT contract: prepare() allocates FFT/window/ring/frame storage and is
    /// not audio-thread safe. After prepare(), process(), analyze(),
    /// synthesize_frame(), available_output(), read_output(), reset(), and
    /// accessors are allocation-free for blocks no larger than max_block and
    /// synthesis hops no larger than max_synthesis_hop. The callback must also
    /// be RT-safe.
    void prepare(const SpectralFrameEngineConfig& config) {
        assert(config.fft_size >= 256 && config.fft_size <= 16384);
        assert((config.fft_size & (config.fft_size - 1)) == 0);
        assert(config.analysis_hop > 0 && config.analysis_hop <= config.fft_size / 2);
        assert(config.channels >= 1);

        config_ = config;
        if (config_.max_synthesis_hop <= 0)
            config_.max_synthesis_hop = 2 * config_.analysis_hop;
        num_bins_ = config_.fft_size / 2 + 1;

        fft_ = Fft(config_.fft_size);
        window_ = WindowFunction::generate(config_.fft_size, config_.window);

        // Steady-state OLA window-energy at a fully-overlapped sample for
        // the analysis hop. Used to floor the per-sample normalization so
        // partial-overlap samples at stream edges taper to zero instead of
        // being amplified by division by a near-zero coverage (pulp #3975).
        // The floor sits far below any real body coverage (down to a 4x
        // sparser synthesis hop), so body samples normalize unchanged.
        double steady = 0.0;
        const int n = config_.fft_size, h = config_.analysis_hop;
        const int center = n; // well inside the plateau
        for (int j = -n / h - 1; j <= n / h + 1; ++j) {
            const int idx = center - j * h - (n / 2);
            if (idx >= 0 && idx < n) {
                const float w = window_[static_cast<size_t>(idx)];
                steady += static_cast<double>(w) * w;
            }
        }
        min_norm_ = std::max(static_cast<float>(steady) * 0.25f, 1e-9f);

        // Output ring must hold the unfinalized span (fft_size) plus the
        // worst-case backlog a caller can create in one process() call.
        const int worst_frames_per_block =
            config_.max_block / config_.analysis_hop + 2;
        ring_size_ = next_pow2(config_.fft_size + config_.max_block
                               + worst_frames_per_block * config_.max_synthesis_hop);
        ring_mask_ = ring_size_ - 1;

        input_ring_.assign(static_cast<size_t>(config_.channels) * config_.fft_size, 0.0f);
        output_ring_.assign(static_cast<size_t>(config_.channels) * ring_size_, 0.0f);
        norm_ring_.assign(ring_size_, 0.0f);

        frames_.assign(static_cast<size_t>(config_.channels) * num_bins_,
                       std::complex<float>(0.0f, 0.0f));
        frame_ptrs_.resize(config_.channels);
        for (int ch = 0; ch < config_.channels; ++ch)
            frame_ptrs_[ch] = frames_.data() + static_cast<size_t>(ch) * num_bins_;

        time_buf_.assign(config_.fft_size, 0.0f);
        freq_buf_.assign(config_.fft_size, std::complex<float>(0.0f, 0.0f));

        reset();
    }

    /// Fixed delay of the equal-hop `process()` path. A frame is only
    /// final once every overlapping frame has been added, which trails the
    /// input by up to fft_size + (analysis_hop - 1) samples depending on
    /// block phase; the constant fft_size + analysis_hop bound makes the
    /// reported latency exact and block-size independent.
    int latency_samples() const { return config_.fft_size + config_.analysis_hop; }

    int fft_size() const { return config_.fft_size; }
    int analysis_hop() const { return config_.analysis_hop; }
    int num_bins() const { return num_bins_; }
    int channels() const { return config_.channels; }

    /// Equal-hop streaming: push `num_samples` per channel, invoke
    /// `on_frames(std::complex<float>* const* frames, int num_bins)` once per
    /// completed analysis frame (modify in place), resynthesize at the
    /// analysis hop, and write exactly `num_samples` per channel to `out`
    /// (the first `latency_samples()` of the stream are zeros).
    template <typename Fn>
    void process(const float* const* in, float* const* out, int num_samples, Fn&& on_frames) {
        assert(num_samples <= config_.max_block);
        analyze(in, num_samples, [&](std::complex<float>* const* frames, int bins) {
            on_frames(frames, bins);
            synthesize_frame(frames, config_.analysis_hop);
        });
        // Fixed-latency read: the first latency_samples() outputs are zeros;
        // afterwards every output pops exactly one final ring sample, so the
        // input→output delay is constant regardless of block size or phase.
        const auto lat = static_cast<std::int64_t>(latency_samples());
        for (int i = 0; i < num_samples; ++i) {
            if (out_count_ < lat) {
                for (int ch = 0; ch < config_.channels; ++ch) out[ch][i] = 0.0f;
            } else {
                pop_one(out, i);
            }
            ++out_count_;
        }
    }

    /// Samples that must still be fed before the next analysis frame
    /// completes (>= 1). Lets a caller chunk its feed so each `analyze`
    /// call ends exactly when a frame emits — making the in-block offset of
    /// the completed frame known precisely (needed to evaluate per-frame
    /// control trajectories, e.g. a smoothed pitch ratio, at the correct
    /// position instead of the chunk end).
    int samples_until_next_frame() const {
        return static_cast<int>(next_frame_at_ - samples_fed_);
    }

    /// Split API — analysis only. Pushes `num_samples` per channel from
    /// `in`, invoking `on_frames` once per completed frame. Runs are split
    /// exactly at frame boundaries, so frames land at fft_size + k * hop
    /// for ANY feed chunking — the analysis hop the callback observes is
    /// constant regardless of host block size.
    template <typename Fn>
    void analyze(const float* const* in, int num_samples, Fn&& on_frames) {
        const int n = config_.fft_size;
        int done = 0;
        while (done < num_samples) {
            const auto until_frame = static_cast<int>(
                std::min<std::int64_t>(next_frame_at_ - samples_fed_,
                                       static_cast<std::int64_t>(num_samples - done)));
            const int run = std::max(until_frame, 1);
            for (int ch = 0; ch < config_.channels; ++ch) {
                float* ring = input_ring_.data() + static_cast<size_t>(ch) * n;
                for (int i = 0; i < run; ++i)
                    ring[(input_pos_ + i) % n] = in[ch][done + i];
            }
            input_pos_ = (input_pos_ + run) % n;
            samples_fed_ += run;
            done += run;

            if (samples_fed_ == next_frame_at_) {
                next_frame_at_ += config_.analysis_hop;
                emit_frame(on_frames);
            }
        }
    }

    /// Split API — overlap-add one spectral frame (all channels) at the
    /// current synthesis position, then advance it by `synthesis_hop`.
    /// `frames` must hold `channels()` pointers to `num_bins()` bins
    /// (DC..Nyquist); the conjugate half is reconstructed internally.
    void synthesize_frame(std::complex<float>* const* frames, int synthesis_hop) {
        assert(synthesis_hop > 0 && synthesis_hop <= config_.max_synthesis_hop);
        const int n = config_.fft_size;
        for (int ch = 0; ch < config_.channels; ++ch) {
            for (int k = 0; k < num_bins_; ++k) {
                freq_buf_[static_cast<size_t>(k)] = frames[ch][k];
                if (k > 0 && k < n / 2)
                    freq_buf_[static_cast<size_t>(n - k)] = std::conj(frames[ch][k]);
            }
            fft_.inverse(freq_buf_.data());
            float* ring = output_ring_.data() + static_cast<size_t>(ch) * ring_size_;
            for (int i = 0; i < n; ++i) {
                const auto idx = static_cast<size_t>((synth_pos_ + i) & ring_mask_);
                ring[idx] += freq_buf_[static_cast<size_t>(i)].real() * window_[static_cast<size_t>(i)];
            }
        }
        for (int i = 0; i < n; ++i) {
            const auto idx = static_cast<size_t>((synth_pos_ + i) & ring_mask_);
            norm_ring_[idx] += window_[static_cast<size_t>(i)] * window_[static_cast<size_t>(i)];
        }
        synth_pos_ += synthesis_hop;
        // Samples before the start of the frame just written are final.
        available_ = std::max(available_, synth_pos_ - synthesis_hop);
    }

    /// Split API — number of final (fully overlapped) output samples that
    /// can be read right now.
    int available_output() const {
        return static_cast<int>(available_ - read_pos_);
    }

    /// Split API — pop exactly `num_samples` per channel into `out`.
    /// Callers should not request more than `available_output()`; any
    /// excess is filled with silence without advancing the read position
    /// (the engine never invents a variable delay on its own).
    void read_output(float* const* out, int num_samples) {
        for (int i = 0; i < num_samples; ++i) {
            if (read_pos_ < available_) {
                pop_one(out, i);
            } else {
                for (int ch = 0; ch < config_.channels; ++ch) out[ch][i] = 0.0f;
            }
        }
    }

    void reset() {
        std::fill(input_ring_.begin(), input_ring_.end(), 0.0f);
        std::fill(output_ring_.begin(), output_ring_.end(), 0.0f);
        std::fill(norm_ring_.begin(), norm_ring_.end(), 0.0f);
        std::fill(frames_.begin(), frames_.end(), std::complex<float>(0.0f, 0.0f));
        input_pos_ = 0;
        samples_fed_ = 0;
        next_frame_at_ = config_.fft_size;
        synth_pos_ = 0;
        available_ = 0;
        read_pos_ = 0;
        out_count_ = 0;
    }

private:
    static int next_pow2(int v) {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    // Pop one final sample (all channels) into out[..][i], normalizing by
    // the accumulated squared synthesis window and clearing the slot.
    void pop_one(float* const* out, int i) {
        assert(read_pos_ < available_);
        const auto idx = static_cast<size_t>(read_pos_ & ring_mask_);
        // Floor only the stream-start partial-overlap region. Once a full FFT
        // window has elapsed, per-sample normalization must remain exact for
        // non-COLA windows/hops whose valid body coverage can dip below the
        // startup floor.
        const bool startup_edge = read_pos_ < config_.fft_size;
        const float norm = startup_edge ? std::max(norm_ring_[idx], min_norm_)
                                        : norm_ring_[idx];
        for (int ch = 0; ch < config_.channels; ++ch) {
            float* ring = output_ring_.data() + static_cast<size_t>(ch) * ring_size_;
            out[ch][i] = norm > 1e-9f ? ring[idx] / norm : 0.0f;
            ring[idx] = 0.0f;
        }
        norm_ring_[idx] = 0.0f;
        ++read_pos_;
    }

    // Window + transform the last fft_size samples of every channel's
    // input ring and hand the frame group to the callback.
    template <typename Fn>
    void emit_frame(Fn&& on_frames) {
        const int n = config_.fft_size;
        for (int ch = 0; ch < config_.channels; ++ch) {
            const float* ring = input_ring_.data() + static_cast<size_t>(ch) * n;
            for (int i = 0; i < n; ++i)
                time_buf_[static_cast<size_t>(i)] =
                    ring[(input_pos_ + i) % n] * window_[static_cast<size_t>(i)];
            fft_.forward_real(time_buf_.data(), freq_buf_.data());
            std::copy(freq_buf_.begin(), freq_buf_.begin() + num_bins_,
                      frames_.begin() + static_cast<size_t>(ch) * num_bins_);
        }
        on_frames(frame_ptrs_.data(), num_bins_);
    }

    SpectralFrameEngineConfig config_;
    Fft fft_{2048};
    std::vector<float> window_;
    int num_bins_ = 0;
    int ring_size_ = 0;
    int ring_mask_ = 0;
    float min_norm_ = 1e-9f;  // OLA coverage floor for edge taper (#3975)

    std::vector<float> input_ring_;     // channels * fft_size
    std::vector<float> output_ring_;    // channels * ring_size
    std::vector<float> norm_ring_;      // ring_size (shared across channels)

    std::vector<std::complex<float>> frames_;            // channels * num_bins
    std::vector<std::complex<float>*> frame_ptrs_;       // channels
    std::vector<float> time_buf_;                        // fft_size
    std::vector<std::complex<float>> freq_buf_;          // fft_size

    int input_pos_ = 0;
    std::int64_t samples_fed_ = 0;
    std::int64_t next_frame_at_ = 0;
    std::int64_t synth_pos_ = 0;   // next frame start (absolute samples)
    std::int64_t available_ = 0;   // final samples high-water mark
    std::int64_t read_pos_ = 0;    // absolute read position
    std::int64_t out_count_ = 0;   // samples emitted by process()
};

} // namespace pulp::signal
