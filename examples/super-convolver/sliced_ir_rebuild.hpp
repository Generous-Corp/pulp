#pragma once

// A BOUNDED, RESUMABLE impulse-response rebuild.
//
// Why this exists: SuperConvolver rebuilds its IR whenever the Size knob moves.
// On a host with threads a background worker owns that rebuild, so the audio
// thread only ever picks the finished IR up through the lock-free
// signal::ConvolverIrSwapper. A browser has no such thread: a WAM module lives
// entirely inside an AudioWorklet and WebCLAP's only control context
// (clap_plugin.on_main_thread) is dispatched from the same worklet, so
// Processor::on_non_realtime_tick() — the host-driven reconcile pump — runs
// BETWEEN render quanta ON THE RENDER THREAD. A full rebuild there (IR
// synthesis, a peak-response FFT over the whole IR, and a partitioned FFT
// re-plan for both channels) measured 15.0 ms in one render callback against a
// 2.667 ms quantum budget (128 frames @ 48 kHz) — an unmissable dropout every
// time the knob moved. Coalescing the rebuild to once per render turn (rather
// than once per parameter message) lowered how OFTEN the dropout fired but left
// it unbounded, which is what processor.hpp's on_non_realtime_tick() contract
// ("a long tick can still make the next quantum late — keep the work bounded")
// forbids.
//
// So the rebuild is TIME-SLICED. Every phase of it is expressed as a stream of
// fixed-cost items, and step() consumes at most `budget` items per call. The OLD
// IR keeps rendering audio the whole time; when the job completes, the finished
// ConvolverIrState is published to the audio thread through the same
// ConvolverIrSwapper the native worker uses, so the swap itself is the existing,
// well-tested lock-free handoff. The audio thread still never builds, allocates
// or frees.
//
// The one part that is not a naturally chunkable stream is the peak-response
// normalization: it needs max|H(f)| over a single DFT of the WHOLE IR (up to
// 2^19 points), and that FFT is 4.5 ms of indivisible work if taken as one call.
// It is decomposed here instead — see PeakResponseScan.

#include <pulp/signal/convolver_messages.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::superconvolver {

/// Bounded-work scan for `max_k |DFT_N(x)[k]|` — the quantity
/// normalize_peak_response() needs and the ONLY reason it runs a full-length
/// FFT.
///
/// A monolithic N-point transform cannot be interrupted, so the DFT is
/// decomposed with the textbook Gentleman–Sande (decimation-in-frequency)
/// identity, stopped early:
///
///     X[2k]   = DFT_{N/2}( x[n] + x[n+N/2] )
///     X[2k+1] = DFT_{N/2}( (x[n] - x[n+N/2]) · W_N^n )
///
/// Applying it L times leaves 2^L contiguous, INDEPENDENT sub-sequences of
/// length M = N/2^L, each of whose M-point DFT yields a subset of X. Both halves
/// of the work are now streams of fixed-cost items: one butterfly (the levels),
/// and one M-point transform (the leaves) — and each leaf is just a call into the
/// existing signal::Fft. Every X[k] appears exactly once across the leaves, only
/// PERMUTED, and a max over all of them does not care about order.
///
/// Exact up to float rounding: the arithmetic is a different association of the
/// same butterflies the monolithic FFT performs, so the peak agrees with
/// Fft::forward_real() to ~1e-7 relative (measured). It is NOT bit-identical, and
/// nothing here relies on it being so.
class PeakResponseScan {
public:
    /// Leaf transform size. Small enough that one leaf is a trivially bounded
    /// unit of work (~7 µs), large enough that the level passes above it are few.
    static constexpr std::size_t kLeaf = 1024;

    /// Begin a scan over `data` (which the caller keeps alive and unmodified).
    /// `n` is the padded, power-of-two transform length; samples past
    /// `data.size()` are the implicit zero padding.
    void start(const std::vector<float>& data, std::size_t n) {
        n_ = n;
        leaf_ = std::min(kLeaf, n_);
        levels_ = 0;
        for (std::size_t m = n_; m > leaf_; m >>= 1) ++levels_;

        buf_.assign(n_, {0.0f, 0.0f});
        twiddle_.assign(n_ / 2 ? n_ / 2 : 1, {1.0f, 0.0f});
        src_ = &data;
        fill_pos_ = 0;
        twiddle_pos_ = 0;
        level_ = 0;
        level_pos_ = 0;
        leaf_pos_ = 0;
        peak_ = 0.0f;
        fft_ = std::make_unique<signal::Fft>(static_cast<int>(leaf_));
        phase_ = Phase::Fill;
    }

    bool done() const { return phase_ == Phase::Done; }
    float peak() const { return peak_; }

    /// Consume at most `budget` items. Returns the number actually consumed
    /// (< budget only when the scan finished inside this call).
    std::size_t step(std::size_t budget) {
        std::size_t spent = 0;
        while (spent < budget && phase_ != Phase::Done) {
            const std::size_t left = budget - spent;
            switch (phase_) {
            case Phase::Fill:      spent += fill(left);      break;
            case Phase::Twiddle:   spent += twiddle(left);   break;
            case Phase::Levels:    spent += levels(left);    break;
            case Phase::Leaves:    spent += leaves(left);    break;
            case Phase::Done:      break;
            }
        }
        return spent;
    }

private:
    enum class Phase { Fill, Twiddle, Levels, Leaves, Done };

    // Copy the (zero-padded) real input into the complex work buffer.
    std::size_t fill(std::size_t left) {
        const std::size_t take = std::min(left, n_ - fill_pos_);
        const std::size_t src_n = src_->size();
        for (std::size_t i = fill_pos_; i < fill_pos_ + take; ++i)
            buf_[i] = {i < src_n ? (*src_)[i] : 0.0f, 0.0f};
        fill_pos_ += take;
        if (fill_pos_ == n_) phase_ = Phase::Twiddle;
        return take;
    }

    // W_N^j for j < N/2. Every level reads this ONE table with a stride
    // (level l uses W_{N>>l}^j == W_N^{j << l}), so it is built once.
    // std::polar is several times the cost of a butterfly, hence the weight.
    static constexpr std::size_t kTwiddleWeight = 4;
    std::size_t twiddle(std::size_t left) {
        const std::size_t total = twiddle_.size();
        const std::size_t take =
            std::min((left + kTwiddleWeight - 1) / kTwiddleWeight, total - twiddle_pos_);
        constexpr float kTwoPi = 6.283185307179586f;
        for (std::size_t j = twiddle_pos_; j < twiddle_pos_ + take; ++j)
            twiddle_[j] = std::polar(1.0f, -kTwoPi * static_cast<float>(j)
                                               / static_cast<float>(n_));
        twiddle_pos_ += take;
        if (twiddle_pos_ == total) phase_ = levels_ ? Phase::Levels : Phase::Leaves;
        return take * kTwiddleWeight;
    }

    // One DIF level = N/2 butterflies, resumable at any butterfly. `level_pos_`
    // is the flat butterfly index within the level, so a slice can end anywhere.
    std::size_t levels(std::size_t left) {
        const std::size_t span = n_ >> level_;      // sub-sequence length at this level
        const std::size_t half = span / 2;
        const std::size_t stride = std::size_t{1} << level_;
        const std::size_t total = n_ / 2;           // butterflies per level
        const std::size_t take = std::min(left, total - level_pos_);
        for (std::size_t b = level_pos_; b < level_pos_ + take; ++b) {
            const std::size_t start = (b / half) * span;
            const std::size_t j = b % half;
            const auto a = buf_[start + j];
            const auto c = buf_[start + j + half];
            buf_[start + j] = a + c;
            buf_[start + j + half] = (a - c) * twiddle_[j * stride];
        }
        level_pos_ += take;
        if (level_pos_ == total) {
            level_pos_ = 0;
            if (++level_ == levels_) phase_ = Phase::Leaves;
        }
        return take;
    }

    // Each leaf is one `leaf_`-point transform; its magnitudes fold into the max.
    // Charged at its real cost (leaf_ items) so a budget means the same thing here
    // as in every other phase.
    std::size_t leaves(std::size_t left) {
        std::size_t spent = 0;
        while (leaf_pos_ < n_ && spent < left) {
            fft_->forward(buf_.data() + leaf_pos_);
            for (std::size_t k = 0; k < leaf_; ++k)
                peak_ = std::max(peak_, std::abs(buf_[leaf_pos_ + k]));
            leaf_pos_ += leaf_;
            spent += leaf_;
        }
        if (leaf_pos_ >= n_) {
            phase_ = Phase::Done;
            buf_.clear();
            buf_.shrink_to_fit();
            twiddle_.clear();
            twiddle_.shrink_to_fit();
            fft_.reset();
        }
        return spent;
    }

    Phase phase_ = Phase::Done;
    const std::vector<float>* src_ = nullptr;
    std::size_t n_ = 0, leaf_ = 0, levels_ = 0;
    std::size_t fill_pos_ = 0, twiddle_pos_ = 0, level_ = 0, level_pos_ = 0, leaf_pos_ = 0;
    float peak_ = 0.0f;
    std::vector<std::complex<float>> buf_;
    std::vector<std::complex<float>> twiddle_;
    std::unique_ptr<signal::Fft> fft_;
};

/// The whole IR rebuild as a resumable job: synthesize (or window) the IR,
/// peak-response normalize it, and build one ready-to-swap
/// signal::ConvolverIrState per channel — all of it in bounded slices.
///
/// The caller (SuperConvolverProcessor) restarts the job whenever the requested
/// Size or IR source changes, which SUPERSEDES any job in flight rather than
/// queueing a second one, and pumps step() once per non-realtime tick. The old
/// IR keeps producing audio until the new one is complete.
class SlicedIrRebuild {
public:
    /// A synthetic built-in IR: `length` samples of seeded, exponentially-decaying
    /// noise (the make_reverb_ir_shaped generator), peak-response normalized.
    void start_synthetic(std::size_t length, float decay_norm, float density,
                         std::uint32_t seed, std::size_t block_size,
                         std::size_t channels) {
        reset(block_size, channels);
        ir_.assign(length, 0.0f);
        decay_norm_ = decay_norm;
        density_ = density;
        lcg_ = seed;
        gen_pos_ = 0;
        normalize_ = true;
        phase_ = length ? Phase::Generate : Phase::Stage;
    }

    /// A loaded base (decoded PCM or file) windowed to `target_len` with a
    /// raised-cosine `fade_len` tail — the same window_ir_to_length() shape. When
    /// the base is already no longer than the target it is taken VERBATIM and NOT
    /// re-normalized, matching window_ir_to_length()'s early return exactly (the
    /// loader already unit-energy normalized it).
    void start_windowed(const std::vector<float>& base, std::size_t target_len,
                        std::size_t fade_len, std::size_t block_size,
                        std::size_t channels) {
        reset(block_size, channels);
        const bool cut = target_len > 0 && base.size() > target_len;
        const std::size_t len = cut ? target_len : base.size();
        ir_.assign(len, 0.0f);
        base_ = &base;
        fade_ = cut ? std::min(fade_len, target_len) : 0;
        normalize_ = cut;
        gen_pos_ = 0;
        phase_ = len ? Phase::Window : Phase::Stage;
    }

    bool active() const { return phase_ != Phase::Idle; }

    /// Advance by at most `budget` work items. Returns true on the call that
    /// COMPLETES the job — at which point ir() and take_state() are ready.
    bool step(std::size_t budget) {
        std::size_t spent = 0;
        while (spent < budget && phase_ != Phase::Idle) {
            const std::size_t left = budget - spent;
            switch (phase_) {
            case Phase::Generate:  spent += generate(left);  break;
            case Phase::Window:    spent += window(left);    break;
            case Phase::Normalize: spent += normalize(left); break;
            case Phase::ApplyGain: spent += apply_gain(left); break;
            case Phase::Stage:     spent += stage(left);     break;
            case Phase::Idle:      break;
            }
            if (phase_ == Phase::Idle) return true;   // just finished
        }
        return false;
    }

    /// Abandon a job in flight (a superseding request, or release()).
    void cancel() {
        phase_ = Phase::Idle;
        ir_.clear();
        states_.clear();
        base_ = nullptr;
    }

    const std::vector<float>& ir() const { return ir_; }
    std::vector<float> take_ir() { return std::move(ir_); }

    /// The finished per-channel state, handed to signal::ConvolverIrSwapper.
    std::unique_ptr<signal::ConvolverIrState> take_state(std::size_t ch) {
        return ch < states_.size() ? std::move(states_[ch]) : nullptr;
    }

private:
    enum class Phase { Idle, Generate, Window, Normalize, ApplyGain, Stage };

    void reset(std::size_t block_size, std::size_t channels) {
        block_ = block_size;
        channels_ = channels;
        ir_.clear();
        states_.clear();
        states_.resize(channels);
        base_ = nullptr;
        stage_ch_ = 0;
        stage_part_ = 0;
        scan_ = PeakResponseScan{};
        gain_pos_ = 0;
        peak_ = 0.0f;
    }

    // exp() per sample; charged above a plain copy so the budget stays honest.
    static constexpr std::size_t kGenWeight = 4;

    // The make_reverb_ir_shaped() generator, resumable. The LCG is sequential, so
    // the job carries its state across slices and any partition of the sample
    // range produces byte-identical samples to one call.
    std::size_t generate(std::size_t left) {
        const std::size_t n = ir_.size();
        const std::size_t take =
            std::min((left + kGenWeight - 1) / kGenWeight, n - gen_pos_);
        const float decay = decay_norm_ / static_cast<float>(n);
        for (std::size_t i = gen_pos_; i < gen_pos_ + take; ++i) {
            lcg_ = lcg_ * 1664525u + 1013904223u;
            const float white = static_cast<float>(lcg_ >> 8) / 8388608.0f - 1.0f;
            float v = white * std::exp(-decay * static_cast<float>(i));
            if (density_ < 1.0f) {
                lcg_ = lcg_ * 1664525u + 1013904223u;
                const float u = static_cast<float>(lcg_ >> 8) / 16777216.0f;
                if (u >= density_) v = 0.0f;
            }
            ir_[i] = v;
        }
        gen_pos_ += take;
        if (gen_pos_ == n) {
            ir_[0] = 1.0f;                     // direct onset
            begin_normalize();
        }
        return take * kGenWeight;
    }

    // Truncate the loaded base to the target length with the raised-cosine fade
    // tail — window_ir_to_length()'s body, resumable.
    std::size_t window(std::size_t left) {
        const std::size_t n = ir_.size();
        const std::size_t take = std::min(left, n - gen_pos_);
        for (std::size_t i = gen_pos_; i < gen_pos_ + take; ++i) {
            float v = (*base_)[i];
            if (fade_ && i >= n - fade_) {
                const float x = static_cast<float>(i - (n - fade_))
                              / static_cast<float>(fade_);
                v *= 0.5f * (1.0f + std::cos(3.14159265f * x));
            }
            ir_[i] = v;
        }
        gen_pos_ += take;
        if (gen_pos_ == n) begin_normalize();
        return take;
    }

    void begin_normalize() {
        if (!normalize_) { phase_ = Phase::Stage; return; }
        if (ir_.size() < 2) {
            // Too short for a meaningful spectrum — unit energy, exactly the
            // fallback normalize_peak_response() takes. O(1) here.
            double energy = 0.0;
            for (float v : ir_) energy += static_cast<double>(v) * v;
            if (energy > 0.0) {
                const float g = static_cast<float>(1.0 / std::sqrt(energy));
                for (float& v : ir_) v *= g;
            }
            phase_ = Phase::Stage;
            return;
        }
        std::size_t nfft = 1;
        while (nfft < ir_.size()) nfft <<= 1;
        scan_.start(ir_, nfft);
        phase_ = Phase::Normalize;
    }

    std::size_t normalize(std::size_t left) {
        const std::size_t spent = scan_.step(left);
        if (scan_.done()) {
            peak_ = scan_.peak();
            gain_pos_ = 0;
            phase_ = peak_ > 0.0f ? Phase::ApplyGain : Phase::Stage;
        }
        return spent ? spent : 1;   // never report zero: the loop must make progress
    }

    std::size_t apply_gain(std::size_t left) {
        const float g = 1.0f / peak_;
        const std::size_t take = std::min(left, ir_.size() - gain_pos_);
        for (std::size_t i = gain_pos_; i < gain_pos_ + take; ++i) ir_[i] *= g;
        gain_pos_ += take;
        if (gain_pos_ == ir_.size()) phase_ = Phase::Stage;
        return take;
    }

    // Build one signal::ConvolverIrState per channel, ONE PARTITION AT A TIME.
    // This is what detail::build_convolver_ir_state() does in a single blocking
    // call (N forward FFTs plus every working buffer); here each partition — its
    // FFT and its share of the working buffers — is a bounded item, so a 375-
    // partition IR spreads across ticks instead of stalling one.
    std::size_t stage(std::size_t left) {
        if (stage_ch_ >= channels_) { phase_ = Phase::Idle; return 0; }

        auto& st = states_[stage_ch_];
        if (!st) {
            st = std::make_unique<signal::ConvolverIrState>();
            st->block_size = static_cast<int>(block_);
            st->fft_size = st->block_size * 2;
            st->fft = std::make_unique<signal::Fft>(st->fft_size);
            st->num_partitions = (ir_.size() + block_ - 1) / block_;
            st->ir_spectra.resize(st->num_partitions);
            st->input_spectra.resize(st->num_partitions);
            st->input_buffer.assign(static_cast<std::size_t>(st->fft_size), {0.0f, 0.0f});
            st->accum.assign(static_cast<std::size_t>(st->fft_size), {0.0f, 0.0f});
            stage_part_ = 0;
        }

        const std::size_t fft_n = static_cast<std::size_t>(st->fft_size);
        std::size_t spent = 0;
        while (stage_part_ < st->num_partitions && spent < left) {
            const std::size_t p = stage_part_;
            const std::size_t offset = p * block_;
            const std::size_t count = std::min(block_, ir_.size() - offset);

            auto& spec = st->ir_spectra[p];
            spec.assign(fft_n, {0.0f, 0.0f});
            for (std::size_t i = 0; i < count; ++i)
                spec[i] = {ir_[offset + i], 0.0f};
            st->fft->forward(spec.data());

            st->input_spectra[p].assign(fft_n, {0.0f, 0.0f});

            ++stage_part_;
            spent += fft_n;
        }

        if (stage_part_ >= st->num_partitions) {
            if (++stage_ch_ >= channels_) phase_ = Phase::Idle;
        }
        return spent ? spent : 1;
    }

    Phase phase_ = Phase::Idle;
    std::size_t block_ = 0, channels_ = 0;

    std::vector<float> ir_;
    const std::vector<float>* base_ = nullptr;
    float decay_norm_ = 0.0f, density_ = 1.0f;
    std::uint32_t lcg_ = 0;
    std::size_t gen_pos_ = 0, fade_ = 0;
    bool normalize_ = true;

    PeakResponseScan scan_;
    float peak_ = 0.0f;
    std::size_t gain_pos_ = 0;

    std::vector<std::unique_ptr<signal::ConvolverIrState>> states_;
    std::size_t stage_ch_ = 0, stage_part_ = 0;
};

}  // namespace pulp::superconvolver
