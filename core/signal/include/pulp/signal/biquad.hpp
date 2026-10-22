#pragma once

#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <type_traits>

namespace pulp::signal {

// Coefficients of one normalized second-order section (a0 == 1).
//
// This is the canonical exchange type between the three layers that all speak
// biquads: design (FilterDesign / IirDesign produce it), runtime (BiquadT
// consumes it via set_coefficients and hands it back via coefficients()), and
// analysis (frequency_response.hpp evaluates H(z) from it). Keeping one shared
// struct is what lets a UI ask a live filter what it looks like instead of
// re-deriving — or approximating — the response itself.
template <typename SampleType = float>
struct BiquadCoefficientsT {
    SampleType b0 = 1, b1 = 0, b2 = 0;
    SampleType a1 = 0, a2 = 0;
};

using BiquadCoefficients = BiquadCoefficientsT<float>;

/// Return whether a normalized real biquad's poles are strictly inside the
/// unit circle. Non-finite coefficients fail the comparisons and are rejected.
template <typename SampleType>
inline bool biquad_is_stable(const BiquadCoefficientsT<SampleType>& c) noexcept {
    // Jury criterion for z^2 + a1*z + a2 with real coefficients.
    if (!(std::abs(c.a2) < SampleType{1}))
        return false;

    // FastTwoSum is exact here because |1| >= |a2|. Keep the residual so a
    // rounded bound equal to |a1| still observes the strict real inequality.
    const SampleType rounded_bound = SampleType{1} + c.a2;
    const SampleType bound_error = c.a2 - (rounded_bound - SampleType{1});
    const SampleType a1_magnitude = std::abs(c.a1);
    return a1_magnitude < rounded_bound
        || (a1_magnitude == rounded_bound && bound_error > SampleType{0});
}

// Biquad IIR filter — standard second-order section.
// RT contract: coefficient calculation and process/reset paths allocate no
// memory. Retune coefficients at a block boundary if parameter continuity
// matters.
template <typename SampleType = float>
class BiquadT {
    static_assert(std::is_floating_point_v<SampleType>,
                  "BiquadT requires a floating-point sample type");

public:
    enum class Type { lowpass, highpass, bandpass, notch, allpass, peaking, low_shelf, high_shelf };

    using Coefficients = BiquadCoefficientsT<SampleType>;

    BiquadT() = default;

    // Load pre-designed coefficients (from FilterDesign, IirDesign, or any
    // other source). Accepts a section of any floating-point precision so a
    // float design table can drive a double-precision filter and vice versa.
    // Assumes the section is already normalized to a0 == 1.
    template <typename U>
    void set_coefficients(const BiquadCoefficientsT<U>& c) {
        b0_ = static_cast<SampleType>(c.b0);
        b1_ = static_cast<SampleType>(c.b1);
        b2_ = static_cast<SampleType>(c.b2);
        a1_ = static_cast<SampleType>(c.a1);
        a2_ = static_cast<SampleType>(c.a2);
        update_block_form();
    }

    // Read back the normalized coefficients currently in force. Lets analysis
    // and UI code evaluate this exact filter's response (see
    // frequency_response.hpp) rather than guessing at its shape.
    Coefficients coefficients() const { return {b0_, b1_, b2_, a1_, a2_}; }

    // Configure from type, frequency, Q, and optional gain (for peaking/shelf)
    void set_coefficients(Type type,
                          SampleType freq_hz,
                          SampleType q,
                          SampleType sample_rate,
                          SampleType gain_db = SampleType{0}) {
        // Guard the inputs that would produce inf/NaN coefficients and wedge the
        // filter state forever: sample_rate ≤ 0 (w0 = ∞), q ≤ 0 (alpha = ∞ at q=0,
        // or a0 = 1+alpha crossing 0 for negative q), and non-finite freq/gain.
        // Clamp only these — any finite positive q, however tiny, yields large but
        // finite coefficients, so every legitimate retune stays bit-identical.
        // Mirrors the input hardening in NoiseGate::set_params.
        if (!(std::isfinite(sample_rate) && sample_rate > SampleType{0}))
            sample_rate = SampleType{44100};
        if (!(std::isfinite(q) && q > SampleType{0}))
            q = SampleType{1e-4f};
        if (!std::isfinite(gain_db)) gain_db = SampleType{0};
        if (!(std::isfinite(freq_hz) && freq_hz > SampleType{0}))
            freq_hz = SampleType{1000};

        SampleType w0 = SampleType{2} * pi * freq_hz / sample_rate;
        SampleType cos_w0 = std::cos(w0);
        SampleType sin_w0 = std::sin(w0);
        SampleType alpha = sin_w0 / (SampleType{2} * q);

        SampleType a0 = SampleType{1};
        SampleType a1 = SampleType{0};
        SampleType a2 = SampleType{0};
        SampleType b0 = SampleType{1};
        SampleType b1 = SampleType{0};
        SampleType b2 = SampleType{0};

        switch (type) {
            case Type::lowpass:
                b0 = (SampleType{1} - cos_w0) / SampleType{2};
                b1 = SampleType{1} - cos_w0;
                b2 = (SampleType{1} - cos_w0) / SampleType{2};
                a0 = SampleType{1} + alpha;
                a1 = -SampleType{2} * cos_w0;
                a2 = SampleType{1} - alpha;
                break;

            case Type::highpass:
                b0 = (SampleType{1} + cos_w0) / SampleType{2};
                b1 = -(SampleType{1} + cos_w0);
                b2 = (SampleType{1} + cos_w0) / SampleType{2};
                a0 = SampleType{1} + alpha;
                a1 = -SampleType{2} * cos_w0;
                a2 = SampleType{1} - alpha;
                break;

            case Type::bandpass:
                b0 = alpha;
                b1 = SampleType{0};
                b2 = -alpha;
                a0 = SampleType{1} + alpha;
                a1 = -SampleType{2} * cos_w0;
                a2 = SampleType{1} - alpha;
                break;

            case Type::notch:
                b0 = SampleType{1};
                b1 = -SampleType{2} * cos_w0;
                b2 = SampleType{1};
                a0 = SampleType{1} + alpha;
                a1 = -SampleType{2} * cos_w0;
                a2 = SampleType{1} - alpha;
                break;

            case Type::allpass:
                b0 = SampleType{1} - alpha;
                b1 = -SampleType{2} * cos_w0;
                b2 = SampleType{1} + alpha;
                a0 = SampleType{1} + alpha;
                a1 = -SampleType{2} * cos_w0;
                a2 = SampleType{1} - alpha;
                break;

            case Type::peaking: {
                SampleType A = std::pow(SampleType{10}, gain_db / SampleType{40});
                b0 = SampleType{1} + alpha * A;
                b1 = -SampleType{2} * cos_w0;
                b2 = SampleType{1} - alpha * A;
                a0 = SampleType{1} + alpha / A;
                a1 = -SampleType{2} * cos_w0;
                a2 = SampleType{1} - alpha / A;
                break;
            }

            case Type::low_shelf: {
                SampleType A = std::pow(SampleType{10}, gain_db / SampleType{40});
                SampleType two_sqrt_a_alpha = SampleType{2} * std::sqrt(A) * alpha;
                b0 = A * ((A + SampleType{1}) - (A - SampleType{1}) * cos_w0 + two_sqrt_a_alpha);
                b1 = SampleType{2} * A * ((A - SampleType{1}) - (A + SampleType{1}) * cos_w0);
                b2 = A * ((A + SampleType{1}) - (A - SampleType{1}) * cos_w0 - two_sqrt_a_alpha);
                a0 = (A + SampleType{1}) + (A - SampleType{1}) * cos_w0 + two_sqrt_a_alpha;
                a1 = -SampleType{2} * ((A - SampleType{1}) + (A + SampleType{1}) * cos_w0);
                a2 = (A + SampleType{1}) + (A - SampleType{1}) * cos_w0 - two_sqrt_a_alpha;
                break;
            }

            case Type::high_shelf: {
                SampleType A = std::pow(SampleType{10}, gain_db / SampleType{40});
                SampleType two_sqrt_a_alpha = SampleType{2} * std::sqrt(A) * alpha;
                b0 = A * ((A + SampleType{1}) + (A - SampleType{1}) * cos_w0 + two_sqrt_a_alpha);
                b1 = -SampleType{2} * A * ((A - SampleType{1}) + (A + SampleType{1}) * cos_w0);
                b2 = A * ((A + SampleType{1}) + (A - SampleType{1}) * cos_w0 - two_sqrt_a_alpha);
                a0 = (A + SampleType{1}) - (A - SampleType{1}) * cos_w0 + two_sqrt_a_alpha;
                a1 = SampleType{2} * ((A - SampleType{1}) - (A + SampleType{1}) * cos_w0);
                a2 = (A + SampleType{1}) - (A - SampleType{1}) * cos_w0 - two_sqrt_a_alpha;
                break;
            }
        }

        // Normalize
        b0_ = b0 / a0;
        b1_ = b1 / a0;
        b2_ = b2 / a0;
        a1_ = a1 / a0;
        a2_ = a2 / a0;
        update_block_form();
    }

    // Process a single sample (Direct Form II Transposed)
    SampleType process(SampleType input) {
        SampleType output = b0_ * input + s1_;
        // Snap the recursive state to zero to keep denormals out of the DF2T
        // feedback path on platforms/threads without an FTZ guard (wasm,
        // MSVC/ARM64, graph worker threads). No-op for any state >= 1e-15.
        s1_ = snap_to_zero(b1_ * input - a1_ * output + s2_);
        s2_ = snap_to_zero(b2_ * input - a2_ * output);
        return output;
    }

    // Process a buffer in-place.
    //
    // The block path evaluates the same DF2T recurrence as the sample path
    // with the feedback products applied last:
    //     y = b0 x + s1;  s1 = (b1 x + s2) - a1 y;  s2 = b2 x - a2 y
    // so the loop-carried path from one output to the next is two
    // multiply-adds instead of four dependent operations. a1 is pre-negated so
    // the two state updates are distinct instructions and are not packed into
    // a two-lane vector, which would put lane moves on that path.
    //
    // The regrouping changes rounding, so block and sample outputs are not
    // bit-identical. The difference is float round-off amplified by the
    // design's noise gain, the same size as the sample path's own error
    // against an exact (float64) DF2T. Over the RBJ type x frequency x Q x
    // gain grid at 44.1/48/96 kHz the block path's RMS error against that
    // exact reference is on average about 1 dB lower than the sample path's
    // and never more than 3 dB higher. Designs with a pole closer to the unit
    // circle than kFeedbackLastMaxPoleRadius keep the sample path's order
    // (see below) and stay bit-identical to it.
    //
    // Coefficients and state are held in locals: the member state would
    // otherwise be stored and reloaded every sample because the buffer may
    // alias it. The denormal snap is applied to the state every
    // kStateSnapInterval samples and at the end of the call instead of after
    // every sample: it only alters states below 1e-15, and a state that starts
    // a chunk at or above 1e-15 cannot decay into the float subnormal range
    // within one chunk unless both poles are inside |z| < 0.44, where the tail
    // falls through the subnormal range to exact zero within a few samples
    // anyway. Subnormal residency is therefore bounded by one chunk however
    // long the buffer is.
    void process(SampleType* buffer, int num_samples) {
        if (sample_path_order_in_block_)
            process_block<false>(buffer, num_samples);
        else
            process_block<true>(buffer, num_samples);
    }

    // Process two channels in place, each through its own filter, with the
    // two recursions in the lanes of one vector so a stereo pair costs about
    // as much as one channel. Each channel's output is bit-identical to
    // calling that filter's block process(); the filters may have different
    // coefficients. Both buffers must hold num_samples samples.
    static void process_stereo(BiquadT& left, BiquadT& right, SampleType* left_buffer,
                               SampleType* right_buffer, int num_samples) {
#if defined(__GNUC__) || defined(__clang__)
        // Below about 64 samples two separate calls already overlap in the
        // out-of-order window and the lane path measured no faster.
        if (num_samples >= kStereoLaneMinSamples &&
            left.sample_path_order_in_block_ == right.sample_path_order_in_block_) {
            if (left.sample_path_order_in_block_)
                process_pair<false>(left, right, left_buffer, right_buffer, num_samples);
            else
                process_pair<true>(left, right, left_buffer, right_buffer, num_samples);
            return;
        }
#endif
        left.process(left_buffer, num_samples);
        right.process(right_buffer, num_samples);
    }

    /// Run a chain of sections (a cascade: each section's output feeds the
    /// next) over a buffer in place.
    ///
    /// Sample-major over up to kChainSectionsPerPass sections at a time: each
    /// sample runs through every section of the pass before the next sample
    /// starts, so the sections' recursions are independent dependency chains
    /// that execute concurrently instead of one section's whole buffer waiting
    /// on its own feedback latency (pipeline interleaving). Every section uses
    /// the sample path's expressions on register-resident coefficients and
    /// state, and the denormal snap runs every kStateSnapInterval samples and
    /// at the end of the call, so the output is identical to chaining the
    /// sections' per-sample process() while the states are at or above 1e-15.
    static void process_chain(BiquadT* sections, std::size_t count, SampleType* buffer,
                              std::size_t num_samples) noexcept {
        std::size_t first = 0;
        while (first < count) {
            const std::size_t n = count - first < kChainSectionsPerPass ? count - first
                                                                         : kChainSectionsPerPass;
            switch (n) {
                case 1: chain_pass<1>(sections + first, buffer, num_samples); break;
                case 2: chain_pass<2>(sections + first, buffer, num_samples); break;
                case 3: chain_pass<3>(sections + first, buffer, num_samples); break;
                case 4: chain_pass<4>(sections + first, buffer, num_samples); break;
                case 5: chain_pass<5>(sections + first, buffer, num_samples); break;
                case 6: chain_pass<6>(sections + first, buffer, num_samples); break;
                case 7: chain_pass<7>(sections + first, buffer, num_samples); break;
                case 8: chain_pass<8>(sections + first, buffer, num_samples); break;
                case 9: chain_pass<9>(sections + first, buffer, num_samples); break;
                case 10: chain_pass<10>(sections + first, buffer, num_samples); break;
                case 11: chain_pass<11>(sections + first, buffer, num_samples); break;
                case 12: chain_pass<12>(sections + first, buffer, num_samples); break;
                case 13: chain_pass<13>(sections + first, buffer, num_samples); break;
                case 14: chain_pass<14>(sections + first, buffer, num_samples); break;
                case 15: chain_pass<15>(sections + first, buffer, num_samples); break;
                default: chain_pass<16>(sections + first, buffer, num_samples); break;
            }
            first += n;
        }
    }

    /// process_chain() for two channels at once, each through its own chain
    /// of `count` sections, with the channels in the lanes of one vector.
    /// Each channel's output is bit-identical to process_chain() on it alone.
    /// Compilers without GNU vector extensions run the two chains in turn.
    static void process_chain_stereo(BiquadT* left, BiquadT* right, std::size_t count,
                                     SampleType* left_buffer, SampleType* right_buffer,
                                     std::size_t num_samples) noexcept {
#if defined(__GNUC__) || defined(__clang__)
        std::size_t first = 0;
        while (first < count) {
            const std::size_t n = count - first < kChainSectionsPerPass ? count - first
                                                                         : kChainSectionsPerPass;
            BiquadT* l = left + first;
            BiquadT* r = right + first;
            switch (n) {
                case 1: chain_pair_pass<1>(l, r, left_buffer, right_buffer, num_samples); break;
                case 2: chain_pair_pass<2>(l, r, left_buffer, right_buffer, num_samples); break;
                case 3: chain_pair_pass<3>(l, r, left_buffer, right_buffer, num_samples); break;
                case 4: chain_pair_pass<4>(l, r, left_buffer, right_buffer, num_samples); break;
                case 5: chain_pair_pass<5>(l, r, left_buffer, right_buffer, num_samples); break;
                case 6: chain_pair_pass<6>(l, r, left_buffer, right_buffer, num_samples); break;
                case 7: chain_pair_pass<7>(l, r, left_buffer, right_buffer, num_samples); break;
                case 8: chain_pair_pass<8>(l, r, left_buffer, right_buffer, num_samples); break;
                case 9: chain_pair_pass<9>(l, r, left_buffer, right_buffer, num_samples); break;
                case 10: chain_pair_pass<10>(l, r, left_buffer, right_buffer, num_samples); break;
                case 11: chain_pair_pass<11>(l, r, left_buffer, right_buffer, num_samples); break;
                case 12: chain_pair_pass<12>(l, r, left_buffer, right_buffer, num_samples); break;
                case 13: chain_pair_pass<13>(l, r, left_buffer, right_buffer, num_samples); break;
                case 14: chain_pair_pass<14>(l, r, left_buffer, right_buffer, num_samples); break;
                case 15: chain_pair_pass<15>(l, r, left_buffer, right_buffer, num_samples); break;
                default: chain_pair_pass<16>(l, r, left_buffer, right_buffer, num_samples); break;
            }
            first += n;
        }
#else
        process_chain(left, count, left_buffer, num_samples);
        process_chain(right, count, right_buffer, num_samples);
#endif
    }

    /// Sections process_chain() runs together in one sample-major pass. A
    /// wider pass keeps more independent recursions in flight; splitting ten
    /// sections into passes of 8 and 2 measured 1.5x slower than one pass, and
    /// sixteen into 8 and 8 1.9x slower.
    static constexpr std::size_t kChainSectionsPerPass = 16;

    // Shortest call process_stereo() runs in vector lanes.
    static constexpr int kStereoLaneMinSamples = 64;

    // Designs whose largest pole radius exceeds this run the block path in
    // the sample path's operation order (bit-identical to it while states
    // stay above 1e-15). Near the unit circle the feedback-last order's RMS
    // error against an exact DF2T grew past the sample path's by more than
    // 3 dB (up to +4 dB for radii 0.995-0.9985 and +9 dB above 0.9995: low
    // shelves, allpasses and notches near 20-100 Hz and high-Q bells); at or
    // below this radius it stayed within +2.8 dB over 5,200 design and
    // stimulus cases.
    static constexpr double kFeedbackLastMaxPoleRadius = 0.995;

    // Number of samples the buffer path runs between denormal snaps of the
    // recursive state.
    static constexpr int kStateSnapInterval = 64;

    // Reset state (call on discontinuities)
    void reset() { s1_ = SampleType{0}; s2_ = SampleType{0}; }

private:
    template <bool FeedbackLast>
    void process_block(SampleType* buffer, int num_samples) {
        const SampleType b0 = b0_, b1 = b1_, b2 = b2_, a1 = a1_, neg_a1 = -a1_, a2 = a2_;
        SampleType s1 = s1_, s2 = s2_;
        for (int start = 0; start < num_samples; start += kStateSnapInterval) {
            const int end = num_samples - start < kStateSnapInterval
                ? num_samples : start + kStateSnapInterval;
            for (int i = start; i < end; ++i) {
                const SampleType input = buffer[i];
                const SampleType output = b0 * input + s1;
                if constexpr (FeedbackLast) {
                    const SampleType t1 = b1 * input + s2;
                    const SampleType t2 = b2 * input;
                    s1 = neg_a1 * output + t1;
                    s2 = t2 - a2 * output;
                } else {
                    s1 = b1 * input - a1 * output + s2;
                    s2 = b2 * input - a2 * output;
                }
                buffer[i] = output;
            }
            s1 = snap_to_zero(s1);
            s2 = snap_to_zero(s2);
        }
        s1_ = s1;
        s2_ = s2;
    }

#if defined(__GNUC__) || defined(__clang__)
    // Two lanes of SampleType; element-wise arithmetic in each lane is the
    // same scalar operation sequence as process_block(), so the lanes match
    // the scalar block path bit for bit.
    using Pair [[gnu::vector_size(2 * sizeof(SampleType))]] = SampleType;

    template <bool FeedbackLast>
    static void process_pair(BiquadT& l, BiquadT& r, SampleType* lb, SampleType* rb,
                             int num_samples) {
        const Pair b0 = {l.b0_, r.b0_}, b1 = {l.b1_, r.b1_}, b2 = {l.b2_, r.b2_};
        const Pair a1 = {l.a1_, r.a1_}, neg_a1 = {-l.a1_, -r.a1_}, a2 = {l.a2_, r.a2_};
        Pair s1 = {l.s1_, r.s1_}, s2 = {l.s2_, r.s2_};
        for (int start = 0; start < num_samples; start += kStateSnapInterval) {
            const int end = num_samples - start < kStateSnapInterval
                ? num_samples : start + kStateSnapInterval;
            for (int i = start; i < end; ++i) {
                const Pair input = {lb[i], rb[i]};
                const Pair output = b0 * input + s1;
                if constexpr (FeedbackLast) {
                    const Pair t1 = b1 * input + s2;
                    const Pair t2 = b2 * input;
                    s1 = neg_a1 * output + t1;
                    s2 = t2 - a2 * output;
                } else {
                    s1 = b1 * input - a1 * output + s2;
                    s2 = b2 * input - a2 * output;
                }
                lb[i] = output[0];
                rb[i] = output[1];
            }
            for (int k = 0; k < 2; ++k) {
                s1[k] = snap_to_zero(s1[k]);
                s2[k] = snap_to_zero(s2[k]);
            }
        }
        l.s1_ = s1[0];
        l.s2_ = s2[0];
        r.s1_ = s1[1];
        r.s2_ = s2[1];
    }
#endif

    template <std::size_t Count>
    static void chain_pass(BiquadT* sections, SampleType* buffer, std::size_t num_samples) noexcept {
        constexpr std::size_t kSnap = static_cast<std::size_t>(kStateSnapInterval);
        SampleType b0[Count], b1[Count], b2[Count], a1[Count], a2[Count], s1[Count], s2[Count];
        for (std::size_t k = 0; k < Count; ++k) {
            b0[k] = sections[k].b0_;
            b1[k] = sections[k].b1_;
            b2[k] = sections[k].b2_;
            a1[k] = sections[k].a1_;
            a2[k] = sections[k].a2_;
            s1[k] = sections[k].s1_;
            s2[k] = sections[k].s2_;
        }
        for (std::size_t start = 0; start < num_samples; start += kSnap) {
            const std::size_t end = num_samples - start < kSnap ? num_samples : start + kSnap;
            for (std::size_t i = start; i < end; ++i) {
                SampleType x = buffer[i];
                for (std::size_t k = 0; k < Count; ++k) {
                    const SampleType output = b0[k] * x + s1[k];
                    s1[k] = b1[k] * x - a1[k] * output + s2[k];
                    s2[k] = b2[k] * x - a2[k] * output;
                    x = output;
                }
                buffer[i] = x;
            }
            for (std::size_t k = 0; k < Count; ++k) {
                s1[k] = snap_to_zero(s1[k]);
                s2[k] = snap_to_zero(s2[k]);
            }
        }
        for (std::size_t k = 0; k < Count; ++k) {
            sections[k].s1_ = s1[k];
            sections[k].s2_ = s2[k];
        }
    }

#if defined(__GNUC__) || defined(__clang__)
    template <std::size_t Count>
    static void chain_pair_pass(BiquadT* l, BiquadT* r, SampleType* lb, SampleType* rb,
                                std::size_t num_samples) noexcept {
        constexpr std::size_t kSnap = static_cast<std::size_t>(kStateSnapInterval);
        Pair b0[Count], b1[Count], b2[Count], a1[Count], a2[Count], s1[Count], s2[Count];
        for (std::size_t k = 0; k < Count; ++k) {
            b0[k] = Pair{l[k].b0_, r[k].b0_};
            b1[k] = Pair{l[k].b1_, r[k].b1_};
            b2[k] = Pair{l[k].b2_, r[k].b2_};
            a1[k] = Pair{l[k].a1_, r[k].a1_};
            a2[k] = Pair{l[k].a2_, r[k].a2_};
            s1[k] = Pair{l[k].s1_, r[k].s1_};
            s2[k] = Pair{l[k].s2_, r[k].s2_};
        }
        for (std::size_t start = 0; start < num_samples; start += kSnap) {
            const std::size_t end = num_samples - start < kSnap ? num_samples : start + kSnap;
            for (std::size_t i = start; i < end; ++i) {
                Pair v = {lb[i], rb[i]};
                for (std::size_t k = 0; k < Count; ++k) {
                    const Pair output = b0[k] * v + s1[k];
                    s1[k] = b1[k] * v - a1[k] * output + s2[k];
                    s2[k] = b2[k] * v - a2[k] * output;
                    v = output;
                }
                lb[i] = v[0];
                rb[i] = v[1];
            }
            for (std::size_t k = 0; k < Count; ++k) {
                for (int lane = 0; lane < 2; ++lane) {
                    s1[k][lane] = snap_to_zero(s1[k][lane]);
                    s2[k][lane] = snap_to_zero(s2[k][lane]);
                }
            }
        }
        for (std::size_t k = 0; k < Count; ++k) {
            l[k].s1_ = s1[k][0];
            l[k].s2_ = s2[k][0];
            r[k].s1_ = s1[k][1];
            r[k].s2_ = s2[k][1];
        }
    }
#endif

    // Largest pole radius of z^2 + a1 z + a2, in double so the threshold
    // decision does not depend on the sample type.
    void update_block_form() {
        const double a1 = static_cast<double>(a1_);
        const double a2 = static_cast<double>(a2_);
        const double disc = a1 * a1 - 4.0 * a2;
        double radius;
        if (disc < 0.0) {
            radius = std::sqrt(a2);
        } else {
            const double root = std::sqrt(disc);
            radius = std::max(std::abs(-a1 + root), std::abs(-a1 - root)) * 0.5;
        }
        // A NaN radius fails the comparison and keeps the sample-path order.
        sample_path_order_in_block_ = !(radius <= kFeedbackLastMaxPoleRadius);
    }

    static constexpr SampleType pi =
        std::is_same_v<SampleType, float>
            ? static_cast<SampleType>(3.14159265358979323846f)
            : static_cast<SampleType>(3.141592653589793238462643383279502884L);

    SampleType b0_ = SampleType{1};
    SampleType b1_ = SampleType{0};
    SampleType b2_ = SampleType{0};
    SampleType a1_ = SampleType{0};
    SampleType a2_ = SampleType{0};
    SampleType s1_ = SampleType{0};
    SampleType s2_ = SampleType{0};
    bool sample_path_order_in_block_ = false;
};

using Biquad = BiquadT<float>;
using Biquad64 = BiquadT<double>;

} // namespace pulp::signal
