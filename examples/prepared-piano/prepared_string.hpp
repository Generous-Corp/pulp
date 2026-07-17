#pragma once

// PreparedString -- a struck modal string with a single-point preparation.
//
// A prepared piano places an object (a bolt, a screw, a rubber wedge) against
// or through a string at a point x_c. This models the string as a bank of
// coupled-form modal resonators (the same rotating-phasor core ModalBankT uses)
// and adds ONE lumped contact at x_c that couples to every mode through its
// shape phi_m(x_c). Three physically-distinct preparations fall out of this one
// structure:
//
//   * Buzz / rattle (a loose bolt): a one-sided collision. The string presses
//     against the object once per cycle when its displacement at x_c exceeds a
//     gap; the restoring force is zero until penetration, then a stiff linear
//     spring, F = -k * max(0, y(x_c) - gap). The rectification -- force present
//     for only part of the cycle -- is what manufactures the high buzz partials
//     and makes them velocity- and position-dependent. Resolved implicitly in
//     closed form every sample (no Newton iteration, no per-note table): the
//     one unknown scalar force is affine in the contact displacement, so it is
//     one compare, one multiply-add and one divide. Bounded for any stiffness,
//     including a rigid barrier (k -> infinity drives y(x_c) exactly to gap).
//
//   * Mute / damp (felt, rubber): raise each mode's decay in proportion to
//     phi_m(x_prep)^2, shortening the modes that have an antinode under the
//     felt and leaving the modes with a node there untouched. This is a
//     control-rate edit to the mode set, not a per-sample cost -- it is applied
//     by the caller before set_modes() and reaches this class as shorter T60s,
//     with the collision disabled (stiffness 0).
//
//   * Mass / detune (a screw threaded through): shift each mode's frequency by
//     -(mu/M) phi_m(x_mass)^2, moving the modes with an antinode under the mass
//     and leaving node modes put. Also a control-rate mode-set edit with the
//     collision disabled. (On a 1-D string a point mass SHIFTS modes rather than
//     splitting them; true splitting needs a 2-D degeneracy to lift. Named
//     accordingly.)
//
// The un-prepared limit (collision disabled, no mode-set edit) is exactly a
// linear stiff string: this reproduces a stiff-string ModalSpec mode-for-mode,
// which is what lets every prepared render be compared against a clean control.
//
// Honest limit: this is a faithful LINEAR modal string plus a lumped point
// contact. It is not a travelling-wave model, so it does not reproduce the
// pluck's interaction with a wave packet in flight -- but the collision, the
// nonlinear part, lives in the modal basis exactly, which is the part a sample
// library cannot hold.
//
// Placement: a header-only sibling of ModalBankT rather than a change to it.
// ModalBankT's inner loop is deliberately elementwise (no cross-mode coupling)
// so it auto-vectorizes; a lumped contact is a per-sample cross-mode reduce
// (read the displacement at x_c) then broadcast (inject the force into every
// mode), which does not fit that loop. The coupled-form coefficients and the
// denormal snap are shared with ModalBankT.
//
// RT contract: prepare() allocates; set_modes(), set_contact(), arm(),
// render() and reset() allocate and lock nothing. set_modes() evaluates a few
// transcendentals per mode (control-rate retune of a few hundred modes).

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/modal_bank.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::examples {

class PreparedString {
public:
    /// Allocate the modal bank for up to @p max_modes modes, a strike-pulse
    /// buffer of @p max_contact_samples, and per-block input scratch of
    /// @p max_block samples. Allocates; call once before processing.
    void prepare(double sample_rate, int max_modes, int max_contact_samples,
                 int max_block) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        const auto n = static_cast<std::size_t>(std::max(max_modes, 1));
        rc_.resize(n);  // AlignedBufferT::resize zero-initializes
        rs_.resize(n);
        cos_w_.resize(n);
        sin_w_.resize(n);
        inj_s_.resize(n);
        inj_c_.resize(n);
        w_p_.resize(n);
        w_c_.resize(n);
        u_.resize(n);
        v_.resize(n);
        us_.resize(n);
        vs_.resize(n);
        pulse_.assign(static_cast<std::size_t>(std::max(max_contact_samples, 1)), 0.0f);
        max_modes_ = static_cast<int>(n);
        mode_count_ = 0;
        pulse_len_ = 0;
        pulse_pos_ = 0;
        ring_remaining_ = 0;
        active_ = false;
        contact_enabled_ = false;
        gap_ = 1.0e30;
        k_ = 0.0;
        g_ = 0.0;
        gp_ = 0.0;
        (void)max_block;
    }

    /// Load a mode set together with the per-mode shape weights of the strike,
    /// pickup and contact points. `modes[m].gain` is the mode's coupling
    /// amplitude (a lumped 1/modal-mass factor); `strike_w`, `pickup_w` and
    /// `contact_w` are phi_m evaluated at the strike, pickup and contact
    /// positions. Preserves resonator state lane-for-lane (retune path), so
    /// striking an already-ringing string layers additively.
    ///
    /// The un-prepared round trip matches ModalBankT: through a unit strike
    /// impulse the pickup reads out gain*strike_w*pickup_w * r^n sin((n+1)w) per
    /// mode -- identical to loading the same resolved modes into ModalBankT.
    void set_modes(std::span<const signal::ModalMode> modes,
                   std::span<const float> strike_w,
                   std::span<const float> pickup_w,
                   std::span<const float> contact_w) {
        const int count = static_cast<int>(
            std::min<std::size_t>(modes.size(), static_cast<std::size_t>(max_modes_)));
        const double fs = sample_rate_;
        double g = 0.0, gp = 0.0;
        for (int m = 0; m < count; ++m) {
            const auto i = static_cast<std::size_t>(m);
            const double freq = std::clamp(static_cast<double>(modes[i].freq_hz), 1.0, 0.49 * fs);
            const double t60 = std::max(static_cast<double>(modes[i].t60_s), 1.0e-3);
            const double r = std::pow(10.0, -3.0 / (t60 * fs));
            const double w = 2.0 * 3.14159265358979323846 * freq / fs;
            const double cw = std::cos(w);
            const double sw = std::sin(w);
            const double coupling = static_cast<double>(modes[i].gain);
            const double ws = i < strike_w.size() ? static_cast<double>(strike_w[i]) : 1.0;
            const double wp = i < pickup_w.size() ? static_cast<double>(pickup_w[i]) : 1.0;
            const double wc = i < contact_w.size() ? static_cast<double>(contact_w[i]) : 0.0;
            rc_[i] = static_cast<float>(r * cw);
            rs_[i] = static_cast<float>(r * sw);
            cos_w_[i] = static_cast<float>(cw);
            sin_w_[i] = static_cast<float>(sw);
            inj_s_[i] = static_cast<float>(coupling * ws);   // strike injection scalar
            inj_c_[i] = static_cast<float>(coupling * wc);   // contact injection scalar
            w_p_[i] = static_cast<float>(wp);                // pickup readout weight
            w_c_[i] = static_cast<float>(wc);                // contact readout weight
            // G  = sum coupling * phi(x_c)^2 * sin(w) : contact-force -> contact-displacement.
            // Gp = sum coupling * phi(x_c) * phi(x_p) * sin(w) : contact-force -> pickup output.
            g += coupling * wc * wc * sw;
            gp += coupling * wc * wp * sw;
        }
        for (int m = count; m < max_modes_; ++m) {
            const auto i = static_cast<std::size_t>(m);
            rc_[i] = rs_[i] = cos_w_[i] = sin_w_[i] = 0.0f;
            inj_s_[i] = inj_c_[i] = w_p_[i] = w_c_[i] = 0.0f;
        }
        g_ = g;
        gp_ = gp;
        mode_count_ = count;
    }

    /// Set the one-sided contact. @p gap is the contact displacement threshold
    /// (the string must swing past it before it touches); @p stiffness is the
    /// linear contact spring constant k. stiffness <= 0 disables the contact
    /// entirely, which is the mute/mass path (a pure linear string). A large
    /// positive gap also means no contact for a normal strike.
    void set_contact(double gap, double stiffness) noexcept {
        gap_ = gap;
        k_ = std::max(stiffness, 0.0);
        contact_enabled_ = k_ > 0.0;
    }

    /// Arm a strike: fill @p contact_samples of the pulse buffer with a
    /// raised-cosine contact of amplitude @p velocity and begin injecting it on
    /// the next render. @p max_t60_s sets how long the string stays active.
    void arm(float velocity, int contact_samples, double max_t60_s) {
        const int n = std::clamp(contact_samples, 1, static_cast<int>(pulse_.size()));
        signal::fill_strike_pulse(
            std::span<float>(pulse_.data(), static_cast<std::size_t>(n)), velocity);
        pulse_len_ = n;
        pulse_pos_ = 0;
        active_ = true;
        ring_remaining_ =
            static_cast<int64_t>(max_t60_s * 2.5 * sample_rate_) + pulse_len_;
    }

    /// Render @p count samples into @p out (accumulating). Drains any armed
    /// strike pulse, advances the modal bank, resolves the contact each sample,
    /// and counts down the active time.
    void render(float* out, int count) {
        if (!active_ || count <= 0 || mode_count_ <= 0) {
            if (active_ && count > 0) {
                ring_remaining_ -= count;
                if (ring_remaining_ <= 0) active_ = false;
            }
            return;
        }
        const int mc = mode_count_;
        const double gain_solve = 1.0 + k_ * g_;   // > 0 for k>=0, g>=0
        const double inv_gain = gain_solve != 0.0 ? 1.0 / gain_solve : 0.0;

        for (int i = 0; i < count; ++i) {
            float f = 0.0f;
            if (pulse_pos_ < pulse_len_) f = pulse_[static_cast<std::size_t>(pulse_pos_++)];

            // Pass 1: advance every mode with the KNOWN excitation only (strike
            // force, no contact yet), accumulating the predicted contact
            // displacement K and predicted pickup output Opred.
            double kk = 0.0, opred = 0.0;
            for (int m = 0; m < mc; ++m) {
                const double ik = static_cast<double>(inj_s_[static_cast<std::size_t>(m)]) * f;
                const double u = u_[static_cast<std::size_t>(m)];
                const double v = v_[static_cast<std::size_t>(m)];
                const double us = rc_[static_cast<std::size_t>(m)] * u
                                - rs_[static_cast<std::size_t>(m)] * v
                                + cos_w_[static_cast<std::size_t>(m)] * ik;
                const double vs = rs_[static_cast<std::size_t>(m)] * u
                                + rc_[static_cast<std::size_t>(m)] * v
                                + sin_w_[static_cast<std::size_t>(m)] * ik;
                us_[static_cast<std::size_t>(m)] = static_cast<float>(us);
                vs_[static_cast<std::size_t>(m)] = static_cast<float>(vs);
                kk += static_cast<double>(w_c_[static_cast<std::size_t>(m)]) * vs;
                opred += static_cast<double>(w_p_[static_cast<std::size_t>(m)]) * vs;
            }

            // Resolve the one-sided contact in closed form. The contact
            // displacement is affine in the unknown force: y_c = K + G*f_c, and
            // the one-sided linear law f_c = -k*[y_c - gap]_+ gives, when
            // penetrating (K > gap so the resolved y_c > gap too),
            //   f_c = -k*(K - gap) / (1 + k*G).
            double fc = 0.0;
            if (contact_enabled_ && kk > gap_)
                fc = -k_ * (kk - gap_) * inv_gain;

            // Pass 2: commit state, adding the resolved contact force into every
            // mode. Output = predicted output + Gp * f_c (folds the contact's
            // pickup contribution without a third reduction).
            if (fc != 0.0) {
                for (int m = 0; m < mc; ++m) {
                    const double extra = static_cast<double>(inj_c_[static_cast<std::size_t>(m)]) * fc;
                    u_[static_cast<std::size_t>(m)] = static_cast<float>(
                        static_cast<double>(us_[static_cast<std::size_t>(m)]) + cos_w_[static_cast<std::size_t>(m)] * extra);
                    v_[static_cast<std::size_t>(m)] = static_cast<float>(
                        static_cast<double>(vs_[static_cast<std::size_t>(m)]) + sin_w_[static_cast<std::size_t>(m)] * extra);
                }
            } else {
                for (int m = 0; m < mc; ++m) {
                    u_[static_cast<std::size_t>(m)] = us_[static_cast<std::size_t>(m)];
                    v_[static_cast<std::size_t>(m)] = vs_[static_cast<std::size_t>(m)];
                }
            }
            out[i] += static_cast<float>(opred + gp_ * fc);
        }

        for (int m = 0; m < mc; ++m) {
            u_[static_cast<std::size_t>(m)] = signal::snap_to_zero(u_[static_cast<std::size_t>(m)]);
            v_[static_cast<std::size_t>(m)] = signal::snap_to_zero(v_[static_cast<std::size_t>(m)]);
        }
        ring_remaining_ -= count;
        if (ring_remaining_ <= 0) active_ = false;
    }

    /// Zero all resonator state and mark the string free (discontinuity reset).
    void reset() {
        std::fill(u_.begin(), u_.end(), 0.0f);
        std::fill(v_.begin(), v_.end(), 0.0f);
        pulse_len_ = 0;
        pulse_pos_ = 0;
        ring_remaining_ = 0;
        active_ = false;
    }

    bool active() const noexcept { return active_; }
    int mode_count() const noexcept { return mode_count_; }
    double sample_rate() const noexcept { return sample_rate_; }
    /// The affine contact-force-to-displacement gain G (recomputed at set_modes).
    double contact_gain() const noexcept { return g_; }

private:
    signal::AlignedBufferT<float> rc_, rs_, cos_w_, sin_w_;
    signal::AlignedBufferT<float> inj_s_, inj_c_, w_p_, w_c_;
    signal::AlignedBufferT<float> u_, v_, us_, vs_;
    std::vector<float> pulse_;

    double sample_rate_ = 48000.0;
    double gap_ = 1.0e30;
    double k_ = 0.0;
    double g_ = 0.0;
    double gp_ = 0.0;
    int max_modes_ = 0;
    int mode_count_ = 0;
    int pulse_len_ = 0;
    int pulse_pos_ = 0;
    int64_t ring_remaining_ = 0;
    bool active_ = false;
    bool contact_enabled_ = false;
};

}  // namespace pulp::examples
