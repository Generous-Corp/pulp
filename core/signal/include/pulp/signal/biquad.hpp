#pragma once

#include <pulp/runtime/dispatch_count.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/simd/lanes.hpp>

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
        history_valid_ = false;
        return output;
    }

    // Process a buffer in-place.
    //
    // The block path runs the same per-pass form selection as process_chain()
    // (a single filter is a one-section pass), so a filter computes the same
    // output whether it is processed alone or as a one-section chain.
    //
    // For float that form is Direct Form I with its recursive state and
    // arithmetic in double, written so the loop-carried dependency from one
    // output to the next is a single multiply-add:
    //     y = -a1 y[n-1] + ((b0 x + b1 x[n-1]) + (b2 x[n-2] - a2 y[n-2]))
    // (y[n-2] reaches y two samples later, so it is off the critical path).
    // That is about twice as fast as the sample path's DF2T order and, with a
    // double accumulator, more accurate: over the RBJ type x frequency x Q x
    // gain grid at 44.1/48/96 kHz with program and white-noise input (2112
    // cases) its RMS error against an exact (float64) DF2T is never above the
    // sample path's and on average 45 dB below it (47 dB without fused
    // multiply-add). Block and sample outputs are therefore not bit-identical:
    // they differ by the sample path's own float round-off, at most -47.9 dBFS
    // over that grid with fused multiply-add and -44.7 dBFS without. (Which
    // compilers fuse is Pulp's contraction policy:
    // tools/cmake/PulpFloatingPointContraction.cmake.)
    //
    // Double SampleType keeps a DF2T block path: the feedback-last order
    //     y = b0 x + s1;  s1 = (-a1) y + (b1 x + s2);  s2 = b2 x - a2 y
    // for designs whose poles are at most kFeedbackLastMaxPoleRadius from the
    // origin, and the sample path's own order (bit-identical to it) above.
    //
    // State continuity: the sample path's DF2T state and the block path's DF1
    // history describe the same filter. A block call that follows sample
    // calls or a coefficient change derives an equivalent DF1 history from
    // the DF2T state (same future output in exact arithmetic), and every
    // block call leaves the DF2T state the sample path will continue from.
    // Retuning therefore has the same (DF2T) transient on either path.
    //
    // Coefficients and state are held in locals: member state would otherwise
    // be stored and reloaded every sample because the buffer may alias it.
    // The denormal snap is applied to the recursive state once per
    // kSnapChunkSamples chunk (see snap_chunks()) instead of after
    // every sample; it only alters states below 1e-15. With both poles inside
    // |z| < 0.44, or with subnormal input, the state can sit in the subnormal
    // range (rounding can hold it in a one-ulp cycle) until the next snap, so
    // subnormal residency is bounded by one chunk however long the buffer is.
    void process(SampleType* buffer, int num_samples) {
        if (num_samples <= 0)
            return;
        dispatch_fixed<1>(this, nullptr, buffer, nullptr, static_cast<std::size_t>(num_samples));
    }

    // Process two channels in place, each through its own filter, with the
    // two recursions in the lanes of one vector so a stereo pair costs about
    // as much as one channel. Each channel's output is bit-identical to
    // calling that filter's block process(); the filters may have different
    // coefficients. Both buffers must hold num_samples samples.
    static void process_stereo(BiquadT& left, BiquadT& right, SampleType* left_buffer,
                               SampleType* right_buffer, std::size_t num_samples) {
        if (num_samples == 0)
            return;
        // Below about 64 samples two separate calls already overlap in the
        // out-of-order window and the lane path measured no faster. Filters
        // whose passes take different forms cannot share lanes.
        if (!simd::kHasVectorLanes || num_samples < kStereoLaneMinSamples ||
            pass_form(&left, 1) != pass_form(&right, 1)) {
            dispatch_fixed<1>(&left, nullptr, left_buffer, nullptr, num_samples);
            dispatch_fixed<1>(&right, nullptr, right_buffer, nullptr, num_samples);
            return;
        }
        dispatch_fixed<1>(&left, &right, left_buffer, right_buffer, num_samples);
    }

    /// Run a chain of sections (a cascade: each section's output feeds the
    /// next) over a buffer in place.
    ///
    /// Sample-major over up to kChainSectionsPerPass sections at a time: each
    /// sample runs through every section of the pass before the next sample
    /// starts, so the sections' recursions are independent dependency chains
    /// that execute concurrently instead of one section's whole buffer waiting
    /// on its own feedback latency (pipeline interleaving). Each pass picks
    /// the fastest form that meets the block path's accuracy contract (see
    /// process(SampleType*, int)); a single filter's block path is the
    /// one-section case of the same choice:
    ///   - float passes of up to kWideStateMaxSections sections run the DF1
    ///     double-state form, with the signal kept in double between the
    ///     sections of the pass;
    ///   - longer passes, and every double pass, run the feedback-last DF2T
    ///     order when every section's largest pole radius is at most
    ///     kFeedbackLastMaxPoleRadius (its RMS error then stays within +2.8 dB
    ///     of the sample path's; per section it differs from the sample path
    ///     by at most -71 dBFS over the RBJ grid), and the sample path's
    ///     order, bit-identical to chaining the sections' per-sample
    ///     process(), otherwise.
    /// Measured on Apple M3 (ns per sample, contraction on): sample order
    /// 3.8 / 6.2 / 4.2 / 4.7 for 1 / 3 / 6 / 8 sections, feedback-last
    /// 2.0 / 2.3 / 2.7 / 3.2, DF1 double 1.9 / 2.2 / 3.7 / 6.0.
    static void process_chain(BiquadT* sections, std::size_t count, SampleType* buffer,
                              std::size_t num_samples) noexcept {
        for (std::size_t first = 0; first < count; first += kChainSectionsPerPass) {
            const std::size_t n = std::min(count - first, kChainSectionsPerPass);
            dispatch_pass(n, sections + first, nullptr, buffer, nullptr, num_samples);
        }
    }

    /// process_chain() for two channels at once, each through its own chain
    /// of `count` sections, with the channels in the lanes of one vector.
    /// Each channel's output is bit-identical to process_chain() on it alone.
    /// Compilers without vector lanes (simd::kHasVectorLanes) run the two
    /// chains in turn.
    static void process_chain_stereo(BiquadT* left, BiquadT* right, std::size_t count,
                                     SampleType* left_buffer, SampleType* right_buffer,
                                     std::size_t num_samples) noexcept {
        for (std::size_t first = 0; first < count; first += kChainSectionsPerPass) {
            const std::size_t n = std::min(count - first, kChainSectionsPerPass);
            // The two channels' passes must take the same form to share
            // lanes; otherwise they run one after the other.
            if (simd::kHasVectorLanes &&
                pass_form(left + first, n) == pass_form(right + first, n)) {
                dispatch_pass(n, left + first, right + first, left_buffer, right_buffer,
                              num_samples);
            } else {
                dispatch_pass(n, left + first, nullptr, left_buffer, nullptr, num_samples);
                dispatch_pass(n, right + first, nullptr, right_buffer, nullptr, num_samples);
            }
        }
    }

    /// Sections process_chain() runs together in one sample-major pass. A
    /// wider pass keeps more independent recursions in flight; splitting ten
    /// sections into passes of 8 and 2 measured 1.5x slower than one pass, and
    /// sixteen into 8 and 8 1.9x slower.
    static constexpr std::size_t kChainSectionsPerPass = 16;

    /// Longest float pass that runs the DF1 double-state form; its extra
    /// double arithmetic makes longer passes slower than the DF2T forms.
    static constexpr std::size_t kWideStateMaxSections = 4;

    // Shortest call process_stereo() runs in vector lanes.
    static constexpr std::size_t kStereoLaneMinSamples = 64;

    // Longer passes (and double SampleType) use the feedback-last DF2T order
    // only when every section's largest pole radius is at most this. Near the
    // unit circle that order's RMS error against an exact DF2T grew past the
    // sample path's by more than 3 dB (up to +4 dB for radii 0.995-0.9985 and
    // +9 dB above 0.9995: low shelves, allpasses and notches near 20-100 Hz
    // and high-Q bells); at or below this radius it stayed within +2.8 dB
    // over 5,200 design and stimulus cases.
    static constexpr double kFeedbackLastMaxPoleRadius = 0.995;

    // Reset state (call on discontinuities)
    void reset() {
        s1_ = SampleType{0};
        s2_ = SampleType{0};
        hx1_ = hx2_ = hy1_ = hy2_ = 0.0;
        history_valid_ = true;
    }

  private:
    enum class BlockForm { sample_order, feedback_last, wide_df1 };

    // The form a pass of `count` sections takes; process() and
    // process_chain() make the same choice per pass (see process_chain()).
    static BlockForm pass_form(const BiquadT* sections, std::size_t count) noexcept {
        if constexpr (std::is_same_v<SampleType, float>) {
            if (count <= kWideStateMaxSections)
                return BlockForm::wide_df1;
        }
        for (std::size_t k = 0; k < count; ++k)
            if (sections[k].sample_path_order_in_block_)
                return BlockForm::sample_order;
        return BlockForm::feedback_last;
    }

    // Run one pass of `count` sections; `right` non-null runs a second
    // channel's sections in the other lane.
    static void dispatch_pass(std::size_t count, BiquadT* left, BiquadT* right,
                              SampleType* left_buffer, SampleType* right_buffer,
                              std::size_t num_samples) noexcept {
        runtime::dispatch_count<kChainSectionsPerPass>(count, [&](auto sections) {
            dispatch_fixed<decltype(sections)::value>(left, right, left_buffer, right_buffer,
                                                      num_samples);
        });
    }

    template <std::size_t Count>
    static void dispatch_fixed(BiquadT* left, BiquadT* right, SampleType* left_buffer,
                               SampleType* right_buffer, std::size_t num_samples) noexcept {
        BlockForm form = pass_form(left, Count);
        if constexpr (std::is_same_v<SampleType, float> && Count <= kWideStateMaxSections) {
            if (form == BlockForm::wide_df1) {
                // A history that cannot be derived (a degenerate section
                // whose DF2T state no DF1 history reproduces) falls back to
                // the sample path's order for this call.
                bool ok = true;
                for (std::size_t k = 0; k < Count; ++k) {
                    ok = left[k].prepare_history() && ok;
                    if (right)
                        ok = right[k].prepare_history() && ok;
                }
                if (ok) {
                    run_pass_lanes<Count, BlockForm::wide_df1>(left, right, left_buffer,
                                                               right_buffer, num_samples);
                    return;
                }
                form = BlockForm::sample_order;
            }
        }
        if (form == BlockForm::feedback_last)
            run_pass_lanes<Count, BlockForm::feedback_last>(left, right, left_buffer, right_buffer,
                                                            num_samples);
        else
            run_pass_lanes<Count, BlockForm::sample_order>(left, right, left_buffer, right_buffer,
                                                           num_samples);
    }

    // One channel in a scalar, or two in the lanes of one vector where the
    // compiler has them.
    template <std::size_t Count, BlockForm Form>
    static void run_pass_lanes(BiquadT* left, BiquadT* right, SampleType* left_buffer,
                               SampleType* right_buffer, std::size_t num_samples) noexcept {
        if (right == nullptr) {
            BiquadT* filters[1] = {left};
            SampleType* buffers[1] = {left_buffer};
            run_pass<Count, Form, 1>(filters, buffers, num_samples);
            return;
        }
        if constexpr (simd::kHasVectorLanes) {
            BiquadT* filters[2] = {left, right};
            SampleType* buffers[2] = {left_buffer, right_buffer};
            run_pass<Count, Form, 2>(filters, buffers, num_samples);
        } else {
            BiquadT* filters[2] = {left, right};
            SampleType* buffers[2] = {left_buffer, right_buffer};
            for (std::size_t lane = 0; lane < 2; ++lane)
                run_pass<Count, Form, 1>(filters + lane, buffers + lane, num_samples);
        }
    }

    // The value a pass computes in: SampleType for the DF2T forms, double for
    // DF1; `Width` channels side by side.
    template <BlockForm Form>
    using PassElement = std::conditional_t<Form == BlockForm::wide_df1, double, SampleType>;

    template <typename E, std::size_t Width> struct PassLanes {
        using type = E;
    };
#if PULP_SIMD_HAS_VECTOR_LANES
    template <typename E> struct PassLanes<E, 2> {
        using type = simd::Lanes<E, 2>;
    };
#endif

    // One section's recurrence on a lane type V. DF2T state is (s1, s2) in
    // (z0, z1):
    //   sample order:  y = b0 x + s1; s1 = b1 x - a1 y + s2; s2 = b2 x - a2 y
    //   feedback last: the same with s1 = (-a1) y + (b1 x + s2), so the only
    //                  operation after y is one multiply-add;
    // DF1 (double) keeps (x1, x2, y1, y2) in (z0, z1, z2, z3):
    //   y = (-a1) y1 + ((b0 x + b1 x1) + (b2 x2 + (-a2) y2)).
    // a1 and a2 arrive in the sign the form's recurrence uses (see
    // run_pass()).
    template <BlockForm Form, typename V>
    static V step(V x, V b0, V b1, V b2, V a1, V a2, V& z0, V& z1, V& z2, V& z3) noexcept {
        if constexpr (Form == BlockForm::wide_df1) {
            const V feed = (b0 * x + b1 * z0) + (b2 * z1 + a2 * z3);
            const V y = a1 * z2 + feed;
            z1 = z0;
            z0 = x;
            z3 = z2;
            z2 = y;
            return y;
        } else if constexpr (Form == BlockForm::feedback_last) {
            (void)z2;
            (void)z3;
            const V y = b0 * x + z0;
            const V t1 = b1 * x + z1;
            const V t2 = b2 * x;
            z0 = a1 * y + t1;
            z1 = t2 - a2 * y;
            return y;
        } else {
            (void)z2;
            (void)z3;
            const V y = b0 * x + z0;
            z0 = b1 * x - a1 * y + z1;
            z1 = b2 * x - a2 * y;
            return y;
        }
    }

    // Snap each lane of a recursive state value.
    template <typename V> static void snap_lanes(V& v) noexcept {
        for (std::size_t lane = 0; lane < simd::lane_count<V>; ++lane)
            simd::set_lane(v, lane, snap_to_zero(simd::get_lane(v, lane)));
    }

    // A sample-major pass of Count sections over Width channels, coefficients
    // and state in local arrays the compiler keeps in registers (separate
    // arrays per field measured faster than an array of per-section structs
    // once a stereo pass spills). Every lane runs the scalar operation
    // sequence, so a channel's output does not depend on Width.
    template <std::size_t Count, BlockForm Form, std::size_t Width>
    static void run_pass(BiquadT* const* filters, SampleType* const* buffers,
                         std::size_t num_samples) noexcept {
        using E = PassElement<Form>;
        using V = typename PassLanes<E, Width>::type;
        constexpr bool df1 = Form == BlockForm::wide_df1;
        V b0[Count], b1[Count], b2[Count], a1[Count], a2[Count];
        V z0[Count], z1[Count], z2[Count], z3[Count];
        for (std::size_t k = 0; k < Count; ++k) {
            for (std::size_t lane = 0; lane < Width; ++lane) {
                const BiquadT& f = filters[lane][k];
                simd::set_lane(b0[k], lane, static_cast<E>(f.b0_));
                simd::set_lane(b1[k], lane, static_cast<E>(f.b1_));
                simd::set_lane(b2[k], lane, static_cast<E>(f.b2_));
                if constexpr (df1) {
                    simd::set_lane(a1[k], lane, -static_cast<double>(f.a1_));
                    simd::set_lane(a2[k], lane, -static_cast<double>(f.a2_));
                    simd::set_lane(z0[k], lane, f.hx1_);
                    simd::set_lane(z1[k], lane, f.hx2_);
                    simd::set_lane(z2[k], lane, f.hy1_);
                    simd::set_lane(z3[k], lane, f.hy2_);
                } else {
                    simd::set_lane(a1[k], lane, Form == BlockForm::feedback_last ? -f.a1_ : f.a1_);
                    simd::set_lane(a2[k], lane, f.a2_);
                    simd::set_lane(z0[k], lane, f.s1_);
                    simd::set_lane(z1[k], lane, f.s2_);
                    z2[k] = z3[k] = V{};
                }
            }
        }
        for (const auto [begin, end] : snap_chunks(num_samples)) {
            for (std::size_t i = begin; i < end; ++i) {
                V v;
                if constexpr (Width == 1)
                    v = static_cast<E>(buffers[0][i]);
                else
                    v = V{static_cast<E>(buffers[0][i]), static_cast<E>(buffers[1][i])};
                for (std::size_t k = 0; k < Count; ++k)
                    v = step<Form>(v, b0[k], b1[k], b2[k], a1[k], a2[k], z0[k], z1[k], z2[k],
                                   z3[k]);
                for (std::size_t lane = 0; lane < Width; ++lane)
                    buffers[lane][i] = static_cast<SampleType>(simd::get_lane(v, lane));
            }
            // The recursive state: (s1, s2) for DF2T, (y1, y2) for DF1.
            for (std::size_t k = 0; k < Count; ++k) {
                snap_lanes(df1 ? z2[k] : z0[k]);
                snap_lanes(df1 ? z3[k] : z1[k]);
            }
        }
        for (std::size_t k = 0; k < Count; ++k) {
            for (std::size_t lane = 0; lane < Width; ++lane) {
                BiquadT& f = filters[lane][k];
                if constexpr (df1) {
                    f.store_history(simd::get_lane(z0[k], lane), simd::get_lane(z1[k], lane),
                                    simd::get_lane(z2[k], lane), simd::get_lane(z3[k], lane));
                } else {
                    f.s1_ = simd::get_lane(z0[k], lane);
                    f.s2_ = simd::get_lane(z1[k], lane);
                    f.history_valid_ = false;
                }
            }
        }
    }

    // Make the DF1 history describe the current DF2T state: if the last
    // writer was the sample path or a coefficient change, derive an
    // equivalent history (zero past inputs where possible) that produces the
    // same future output in exact arithmetic. Returns false for a degenerate
    // section whose state no history reproduces.
    bool prepare_history() noexcept {
        if (history_valid_)
            return true;
        const double b1 = b1_, b2 = b2_, a1 = a1_, a2 = a2_;
        const double s1 = s1_, s2 = s2_;
        // s2 = b2 x1 - a2 y1;  s1 = b1 x1 - a1 y1 + b2 x2 - a2 y2
        if (a2 != 0.0) {
            hx1_ = 0.0;
            hx2_ = 0.0;
            hy1_ = -s2 / a2;
            hy2_ = (-a1 * hy1_ - s1) / a2;
        } else if (b2 != 0.0) {
            hy1_ = 0.0;
            hy2_ = 0.0;
            hx1_ = s2 / b2;
            hx2_ = (s1 - b1 * hx1_) / b2;
        } else if (s2 != 0.0) {
            return false;
        } else if (a1 != 0.0) {
            hx1_ = 0.0;
            hx2_ = 0.0;
            hy1_ = -s1 / a1;
            hy2_ = 0.0;
        } else if (b1 != 0.0) {
            hy1_ = 0.0;
            hy2_ = 0.0;
            hx1_ = s1 / b1;
            hx2_ = 0.0;
        } else if (s1 != 0.0) {
            return false;
        } else {
            hx1_ = hx2_ = hy1_ = hy2_ = 0.0;
        }
        if (!(std::isfinite(hy1_) && std::isfinite(hy2_) && std::isfinite(hx1_) &&
              std::isfinite(hx2_)))
            return false;
        history_valid_ = true;
        return true;
    }

    // Record a DF1 block's history and the equivalent DF2T state the sample
    // path continues from.
    void store_history(double x1, double x2, double y1, double y2) noexcept {
        hx1_ = x1;
        hx2_ = x2;
        hy1_ = y1;
        hy2_ = y2;
        const double b1 = b1_, b2 = b2_, a1 = a1_, a2 = a2_;
        s2_ = snap_to_zero(static_cast<SampleType>(b2 * x1 - a2 * y1));
        s1_ = snap_to_zero(static_cast<SampleType>(b1 * x1 - a1 * y1 + b2 * x2 - a2 * y2));
        history_valid_ = true;
    }

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
        // New coefficients: the next DF1 block re-derives its history from
        // the DF2T state, so a retune has the same transient on both paths.
        history_valid_ = false;
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
    // DF1 history (last two inputs and outputs) for the float block path,
    // and whether it describes the current DF2T state.
    double hx1_ = 0.0, hx2_ = 0.0, hy1_ = 0.0, hy2_ = 0.0;
    bool history_valid_ = true;
};

using Biquad = BiquadT<float>;
using Biquad64 = BiquadT<double>;

} // namespace pulp::signal
