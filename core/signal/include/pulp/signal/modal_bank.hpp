#pragma once

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/simd_buffer.hpp>

#include <algorithm>
#include <cmath>
#include <span>

namespace pulp::signal {

/// One resonant mode of a vibrating object: frequency, decay time, and
/// impulse-response amplitude. A modal model of a struck/plucked body is a
/// (possibly large) set of these, typically produced offline by modal
/// analysis of a measurement or a mesh and played back by ModalBankT.
struct ModalMode {
    float freq_hz = 440.0f;
    float t60_s = 1.0f;   ///< time for this mode to decay by 60 dB
    float gain = 1.0f;    ///< amplitude of this mode in the impulse response
};

/// Fill @p dst with a raised-cosine contact pulse, peak-normalized to
/// @p velocity. The span length is the contact duration in samples: a short
/// contact (hard mallet) has a flat excitation spectrum that reaches the
/// high modes; a long contact (soft mallet) rolls the high modes off. Feed
/// the pulse into ModalBankT::process_add as input to strike the object.
template <typename SampleType = float>
inline void fill_strike_pulse(std::span<SampleType> dst, SampleType velocity) {
    const auto n = dst.size();
    if (n == 0) return;
    if (n == 1) { dst[0] = velocity; return; }
    constexpr SampleType two_pi = SampleType(6.28318530717958647692);
    const SampleType phase_step = two_pi / static_cast<SampleType>(n);
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = velocity * SampleType(0.5)
               * (SampleType(1) - std::cos(phase_step * static_cast<SampleType>(i)));
    }
}

/// The spatial eigenfunction of one mode along a body's normalized length
/// coordinate x in [0, 1]: phi(x) = sin(pi * half_waves * x + end_phase).
///
/// How hard a strike at x excites a mode, and how loudly a pickup at x hears
/// it, are both proportional to phi(x) — striking a node (phi = 0) does not
/// excite that mode at all, which is why a string struck at its midpoint has
/// no even harmonics.
///
/// This is the sinusoidal-eigenfunction family: ideal strings and the
/// standing-wave idealization of pipes. Set half_waves = m (mode index) and
/// end_phase = 0 for a string fixed at both ends; end_phase = pi/2 puts an
/// antinode at x = 0 for a free end. Bodies whose eigenfunctions are NOT
/// sinusoidal — Euler-Bernoulli bars, plates, shells, anything from a mesh
/// solve — must not be forced into this form: compute their phi(x) with the
/// solver that produced the modes and hand ModalBankT the resulting weights
/// directly. The bank consumes weights, never positions, for this reason.
struct ModalShape {
    float half_waves = 1.0f;  ///< number of half-wavelengths across the body
    float end_phase = 0.0f;   ///< 0 puts a node at x = 0; pi/2 an antinode
};

/// Evaluate a mode's shape at a normalized position along the body.
///
/// Evaluated in double: the argument grows with the mode index, so a float
/// sin() leaves a high mode's nodes progressively less null (about 1e-4 of
/// full scale by mode 1000). A node that is only approximately a node defeats
/// the point of striking one, and this is control-rate arithmetic.
inline float mode_shape_at(const ModalShape& shape, float position) {
    constexpr double pi = 3.14159265358979323846;
    return static_cast<float>(std::sin(pi * static_cast<double>(shape.half_waves)
                                            * static_cast<double>(position)
                                       + static_cast<double>(shape.end_phase)));
}

/// Fill @p dst with the mode shapes of an ideal string fixed at both ends:
/// dst[i] describes the (i+1)-th transverse mode, phi_m(x) = sin(m*pi*x),
/// whose nodes are at x = k/m. Pair with fill_mode_weights() to turn a
/// strike or pickup position into per-mode weights.
inline void fill_ideal_string_shapes(std::span<ModalShape> dst) {
    for (std::size_t i = 0; i < dst.size(); ++i)
        dst[i] = ModalShape{static_cast<float>(i + 1), 0.0f};
}

/// Sample every shape in @p shapes at @p position, writing phi_m(position)
/// to @p dst. Multiply the result into ModalMode::gain for the strike side,
/// or hand it to ModalBankT::set_pickup_gains() for the listening side; the
/// two sides are independent, matching the modal expansion's separable
/// phi_m(strike) * phi_m(pickup) coupling.
template <typename SampleType = float>
inline void fill_mode_weights(std::span<SampleType> dst,
                              std::span<const ModalShape> shapes, float position) {
    const std::size_t n = std::min(dst.size(), shapes.size());
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = static_cast<SampleType>(mode_shape_at(shapes[i], position));
    for (std::size_t i = n; i < dst.size(); ++i) dst[i] = SampleType{0};
}

/// Bank of two-pole modal resonators, stored structure-of-arrays in lane
/// groups so the per-sample state update auto-vectorizes across modes.
///
/// Each mode is a coupled-form (rotating-phasor) resonator: the state pair
/// (u, v) is multiplied every sample by the scaled rotation
/// [rc -rs; rs rc], rc = r*cos(w), rs = r*sin(w), with pole radius
/// r = 10^(-3 / (t60 * fs)) and pole angle w = 2*pi*f/fs. Input is injected
/// as gain*(cos(w), sin(w)) and v is read out, which makes the mode's
/// impulse response exactly gain * r^n * sin((n+1) w) — so through a
/// unit-gain pickup (the default), ModalMode::gain reads directly as the
/// mode's amplitude in the rendered impulse response, and t60_s as its
/// measured decay.
///
/// The injection pair is proportional to the rotation pair — gain*cos(w) =
/// (gain/r)*rc — so one coefficient could replace the two. Do not: folding
/// them costs throughput rather than saving it. Injecting into u ahead of the
/// rotation puts an add on the u -> u' dependency chain, and this loop is
/// latency-bound on that chain, not throughput-bound on its arithmetic. Kept
/// separate, gain*cos(w)*x - rs*v evaluates off the chain and u -> u' folds
/// into a single FMA (measured: folding costs 30% at one pickup and 3.5x at
/// four).
///
/// The coupled form is chosen over the cheaper direct form (y = b0 x +
/// a1 y[n-1] + a2 y[n-2]) for two measured reasons, both about the state's
/// physical meaning. Direct-form state is a pair of past output samples, so
/// the amplitude it implies depends on the coefficients that produced it:
/// retuning under a held state silently rescales the ringing mode by up to
/// w_old/w_new, phase-dependently (a 220 -> 440 Hz retune measures anywhere
/// from 0.50x to 1.00x depending on where in the cycle it lands, and a
/// three-octave glide loses 63% of the mode). Coupled-form state is the
/// phasor itself; the rotation's singular values are both r regardless of w,
/// so a retune preserves amplitude exactly (measured 0.999x, glide within
/// 0.9% of the ideal decay) and no state fixup is needed. Second, the
/// direct form encodes frequency in a1 = 2 r cos(w), which crowds against
/// 2.0 as w -> 0 and loses it to float rounding: a 20 Hz / 8 s mode lands
/// 7.5 cents flat, a 30 Hz / 5 s mode 2.5 cents flat. The coupled form
/// carries frequency in rs = r sin(w), which keeps full relative precision
/// at low w — the same modes land within 0.001 cents. Both forms resolve
/// t60 to better than 0.15% in float.
///
/// The process loop is mode-major: each group of kLanes modes keeps its
/// coefficients and state in local fixed-size arrays across the whole block,
/// so per-block memory traffic is one pass over the state arrays plus the
/// L1-resident input/output buffers, and the lane loops are elementwise
/// (no cross-lane dependence) which vectorizes without -ffast-math.
///
/// Decayed state is snapped to zero at block boundaries, so a silent bank
/// settles to exact zeros and cannot park denormals in the recursion across
/// blocks. Within a block on x86, hold a ScopedFlushDenormals in the caller
/// (as with the other recursive processors) if denormal stalls matter.
///
/// RT contract: prepare() allocates; set_modes(), set_pickup_gains(),
/// process_add(), and reset() allocate no memory after prepare().
/// set_modes() evaluates transcendentals per mode — cheap enough for
/// control-rate retuning of a few hundred modes, but retune large banks from
/// a non-RT thread.
template <typename SampleType = float>
class ModalBankT {
public:
    /// Lane-group width. 16 floats = one cache line; the per-group local
    /// arrays fit the vector register file on NEON and AVX targets.
    static constexpr int kLanes = 16;

    /// Upper bound on simultaneous pickups. Each pickup adds a multiply and
    /// a reduction per mode-sample, and its lane block is held in registers
    /// across the block, so this is deliberately small: pickups model
    /// listening positions on one body, not a mixer.
    static constexpr int kMaxPickups = 8;

    /// Allocate storage for up to @p max_modes modes and @p max_pickups
    /// listening positions at @p sample_rate. Every pickup starts at unit
    /// gain on every mode, so a freshly prepared bank reads out its modes'
    /// ModalMode::gain directly.
    void prepare(double sample_rate, int max_modes, int max_pickups = 1) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        max_modes_ = max_modes > 0 ? max_modes : 0;
        max_pickups_ = std::clamp(max_pickups, 1, kMaxPickups);
        padded_ = static_cast<std::size_t>((max_modes_ + kLanes - 1) / kLanes) * kLanes;
        cu_.resize(padded_);
        cv_.resize(padded_);
        rc_.resize(padded_);
        rs_.resize(padded_);
        u_.resize(padded_);
        v_.resize(padded_);
        pickup_.resize(padded_ * static_cast<std::size_t>(max_pickups_));
        std::fill(pickup_.begin(), pickup_.end(), SampleType{1});
        mode_count_ = 0;
    }

    /// Load a mode set (up to the prepared max; excess modes are ignored).
    ///
    /// This is also the retune path: existing resonator state is preserved
    /// lane-for-lane, and because that state is the mode's phasor rather than
    /// a pair of past outputs, retuning a ringing bank at block rate changes
    /// its pitch without stepping its amplitude and without a state fixup.
    /// Call reset() for a clean start instead.
    void set_modes(std::span<const ModalMode> modes) {
        const int count = static_cast<int>(
            std::min<std::size_t>(modes.size(), static_cast<std::size_t>(max_modes_)));
        const double fs = sample_rate_;
        for (int m = 0; m < count; ++m) {
            const auto i = static_cast<std::size_t>(m);
            const double freq = std::clamp(static_cast<double>(modes[i].freq_hz),
                                           1.0, 0.49 * fs);
            const double t60 = std::max(static_cast<double>(modes[i].t60_s), 1.0e-3);
            const double r = std::pow(10.0, -3.0 / (t60 * fs));
            const double w = 2.0 * 3.14159265358979323846 * freq / fs;
            const double cos_w = std::cos(w);
            const double sin_w = std::sin(w);
            const double gain = static_cast<double>(modes[i].gain);
            rc_[i] = static_cast<SampleType>(r * cos_w);
            rs_[i] = static_cast<SampleType>(r * sin_w);
            cu_[i] = static_cast<SampleType>(gain * cos_w);
            cv_[i] = static_cast<SampleType>(gain * sin_w);
        }
        // Zero the coefficients of unused padding lanes so they contribute
        // exact silence regardless of stale state.
        for (std::size_t m = static_cast<std::size_t>(count); m < padded_; ++m) {
            cu_[m] = SampleType{0};
            cv_[m] = SampleType{0};
            rc_[m] = SampleType{0};
            rs_[m] = SampleType{0};
        }
        mode_count_ = count;
    }

    /// Set how loudly pickup @p channel hears each mode. @p gains is indexed
    /// by mode; modes past its end keep their previous gain. Weights from
    /// fill_mode_weights() put the pickup at a position on the body.
    void set_pickup_gains(int channel, std::span<const SampleType> gains) {
        if (channel < 0 || channel >= max_pickups_) return;
        const std::size_t base = static_cast<std::size_t>(channel) * padded_;
        const std::size_t n = std::min(gains.size(), padded_);
        for (std::size_t m = 0; m < n; ++m) pickup_[base + m] = gains[m];
    }

    /// Excite and run the bank into one output: adds pickup @p channel's
    /// view of the summed mode outputs for @p num_samples of @p input into
    /// @p output (accumulating, so several banks can layer into one voice
    /// buffer). Pass a zero input to ring out.
    void process_add(const SampleType* input, SampleType* output, int num_samples,
                     int channel = 0) {
        SampleType* outs[1] = {output};
        const int c = std::clamp(channel, 0, max_pickups_ - 1);
        run(input, outs, &c, 1, num_samples);
    }

    /// Excite and run the bank into @p num_channels outputs, one per pickup:
    /// pickup c accumulates into outputs[c]. One pass over the resonator
    /// state serves every pickup, which is the point — the body is simulated
    /// once and listened to from several places. Channels past the prepared
    /// max_pickups are ignored and their outputs left untouched.
    void process_add(const SampleType* input, SampleType* const* outputs,
                     int num_channels, int num_samples) {
        int channels[kMaxPickups];
        const int n = std::clamp(num_channels, 0, max_pickups_);
        for (int c = 0; c < n; ++c) channels[c] = c;
        run(input, outputs, channels, n, num_samples);
    }

    /// Zero all resonator state (call on discontinuities / re-strike from
    /// silence). Pickup gains and modes are unaffected.
    void reset() {
        u_.clear();
        v_.clear();
    }

    int mode_count() const { return mode_count_; }
    int max_modes() const { return max_modes_; }
    int max_pickups() const { return max_pickups_; }
    double sample_rate() const { return sample_rate_; }

private:
    void run(const SampleType* input, SampleType* const* outputs,
             const int* channels, int num_channels, int num_samples) {
        if (num_channels <= 0) return;
        const int groups = (mode_count_ + kLanes - 1) / kLanes;
        for (int g = 0; g < groups; ++g) {
            const std::size_t base = static_cast<std::size_t>(g) * kLanes;
            SampleType cu[kLanes], cv[kLanes], rc[kLanes], rs[kLanes];
            SampleType u[kLanes], v[kLanes];
            SampleType pick[kMaxPickups][kLanes];
            for (int l = 0; l < kLanes; ++l) {
                const std::size_t i = base + static_cast<std::size_t>(l);
                cu[l] = cu_[i];
                cv[l] = cv_[i];
                rc[l] = rc_[i];
                rs[l] = rs_[i];
                u[l] = u_[i];
                v[l] = v_[i];
            }
            for (int c = 0; c < num_channels; ++c) {
                const std::size_t pbase =
                    static_cast<std::size_t>(channels[c]) * padded_ + base;
                for (int l = 0; l < kLanes; ++l) pick[c][l] = pickup_[pbase + l];
            }
            for (int i = 0; i < num_samples; ++i) {
                const SampleType x = input[i];
                SampleType y[kLanes];
                for (int l = 0; l < kLanes; ++l) {
                    const SampleType nu = rc[l] * u[l] - rs[l] * v[l] + cu[l] * x;
                    const SampleType nv = rs[l] * u[l] + rc[l] * v[l] + cv[l] * x;
                    u[l] = nu;
                    v[l] = nv;
                    y[l] = nv;
                }
                for (int c = 0; c < num_channels; ++c) {
                    SampleType t[kLanes];
                    for (int l = 0; l < kLanes; ++l) t[l] = pick[c][l] * y[l];
                    // Fixed pairwise reduction order: deterministic output and
                    // vector-width friendly (elementwise adds, then one scalar).
                    for (int step = kLanes / 2; step > 0; step /= 2)
                        for (int l = 0; l < step; ++l) t[l] += t[l + step];
                    outputs[c][i] += t[0];
                }
            }
            for (int l = 0; l < kLanes; ++l) {
                const std::size_t i = base + static_cast<std::size_t>(l);
                u_[i] = snap_to_zero(u[l]);
                v_[i] = snap_to_zero(v[l]);
            }
        }
    }

    AlignedBufferT<SampleType> cu_, cv_, rc_, rs_, u_, v_, pickup_;
    double sample_rate_ = 48000.0;
    std::size_t padded_ = 0;
    int max_modes_ = 0;
    int max_pickups_ = 1;
    int mode_count_ = 0;
};

using ModalBank = ModalBankT<float>;
using ModalBank64 = ModalBankT<double>;

} // namespace pulp::signal
