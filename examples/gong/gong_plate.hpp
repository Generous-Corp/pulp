#pragma once

// GongPlate -- a struck thin-plate modal cloud with a bounded nonlinear bloom.
//
// A gong/tam-tam is a thin circular metal plate. Its free bending vibration is
// dispersive (omega proportional to k^2, unlike a membrane's omega ~ k), so its
// modes spread out INHARMONIC -- not n*f0 -- into a dense shimmering cloud that
// rings for many seconds. The linear part of that is exactly what a modal bank
// does, and hundreds of modes are trivial for it.
//
// The gong's signature, though, is NONLINEAR: struck hard, the spectrum BLOOMS
// -- energy cascades from the low modes UP to high modes over ~1-3 s (the swell
// builds AFTER the attack, it is not there at t=0). This is the von Karman
// geometric (large-amplitude) plate nonlinearity: mode amplitudes couple and
// pump energy upward, most strongly at near-resonances omega_h ~ omega_l+omega_m.
// It is amplitude-dependent (a soft strike barely blooms; a hard strike blooms
// hard), which is exactly the behaviour no FFT->process->IFFT pipeline can
// express -- the one honest real-time GPU-audio candidate.
//
// GongPlate implements that coupling ON CPU as a BLOCK-RATE, exactly-energy-
// conserving three-wave kinetic (Manley-Rowe) transfer over a precomputed,
// partner-capped near-resonant sum-triad set:
//
//   * The linear ringing stays at sample rate in a coupled-form modal bank
//     (the same rotating-phasor core ModalBankT uses -- retune-amplitude-safe).
//   * The nonlinearity only REDISTRIBUTES existing modal energy, at block rate.
//     The cascade envelope has a bandwidth of a few Hz, so a block rate of tens
//     to hundreds of Hz massively oversamples it. Cost is O(K) per block, K a
//     fixed prepare()-time ceiling (M * max_partners).
//
// Boundedness is a HARD INVARIANT, not a hope: the coupling pass conserves total
// modal energy exactly (energy removed from the donor pair is added, bit-for-bit,
// to the receiver) and every energy is clamped >= 0, so it is non-amplifying;
// the linear T60 then strictly dissipates. Therefore E_total(t) is monotone
// non-increasing regardless of strike hardness or bloom amount -- a max-hardness
// strike can only rearrange the finite deposited energy and then decay. It
// cannot blow up over 10 s. This is asserted by a test.
//
// Placement: a header-only sibling of ModalBankT (like PreparedString), not a
// change to it. ModalBankT's inner loop is deliberately elementwise so it
// auto-vectorizes; the coupling is a cross-mode read/modify/write on the phasor
// state between blocks, which does not belong in that loop. The coupled-form
// coefficient math and the denormal snap are shared with ModalBankT.
//
// Modeling honesty: the kinetic form captures the ENERGY CASCADE / spectral
// bloom (the audible tam-tam signature) but deliberately discards coherent phase
// dynamics -- it is not a route-to-chaos model of a DRIVEN cymbal. For a STRUCK
// gong the transient is dominated by energy redistribution, so this is the right
// level of model. The coupling weights are a physically-motivated plausibility
// choice (stronger for closer resonance), tuned against the measured centroid
// bloom, not a calibrated von Karman overlap-integral solve.
//
// RT contract: prepare() and set_modes() allocate (set_modes rebuilds the triad
// list -- a mode-set change, never per note or per Tune). render(), retune-free
// control setters, and reset() allocate and lock nothing. The per-block work is
// bounded: M sample-rate updates + K = M*max_partners block-rate triad evals.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/modal_bank.hpp>
#include <pulp/signal/simd_buffer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::examples {

class GongPlate {
public:
    /// Lane-group width for the linear ring loop (one cache line of floats),
    /// matching ModalBankT so the sample-rate update auto-vectorizes.
    static constexpr int kLanes = 16;

    /// Fixed coupling sub-block: the linear ring advances this many samples,
    /// then one energy-transfer pass runs. Keeping it fixed (independent of the
    /// host block size) makes the cascade rate and the RT cost deterministic.
    /// 256 samples = ~188 Hz at 48 kHz, ~30x over the few-Hz bloom envelope.
    static constexpr int kCouplingBlock = 256;

    /// Allocate for up to @p max_modes modes, @p max_partners coupling partners
    /// per mode (the triad-count ceiling is max_modes*max_partners), and a
    /// per-render input scratch of @p max_block samples. Allocates; call once.
    void prepare(double sample_rate, int max_modes, int max_partners = 16,
                 int max_block = 512) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        max_modes_ = std::max(max_modes, 1);
        max_partners_ = std::max(max_partners, 0);
        padded_ = (static_cast<std::size_t>(max_modes_) + kLanes - 1) / kLanes * kLanes;

        cu_.resize(padded_);
        cv_.resize(padded_);
        rc_.resize(padded_);
        rs_.resize(padded_);
        u_.resize(padded_);
        v_.resize(padded_);
        w_out_.resize(padded_);
        freq_.resize(padded_);
        omega_.resize(padded_);
        r_.resize(padded_);
        r_base_.resize(padded_);
        gain_.resize(padded_);
        freq_base_.resize(padded_);

        const std::size_t max_triads =
            static_cast<std::size_t>(max_modes_) * static_cast<std::size_t>(max_partners_) + 1;
        tl_.assign(max_triads, 0);
        tm_.assign(max_triads, 0);
        th_.assign(max_triads, 0);
        tk_.assign(max_triads, 0.0f);
        part_count_.assign(static_cast<std::size_t>(max_modes_), 0);

        mode_count_ = 0;
        triad_count_ = 0;
        bloom_ = 0.0;
        (void)max_block;  // excitation is caller-owned; render() takes external input
    }

    /// Load a mode set (frequency, T60, strike gain per mode) and a per-mode
    /// output readout weight (the Tone-tilted pickup; pass empty for unit).
    /// Rebuilds the near-resonant triad set. Preserves resonator state
    /// lane-for-lane (retune / additive-strike path). Allocation happens only
    /// in the triad rebuild scratch; this is a control-thread / mode-set-change
    /// operation, not a per-note one.
    void set_modes(std::span<const signal::ModalMode> modes,
                   std::span<const float> out_weights = {}) {
        const int count = static_cast<int>(
            std::min<std::size_t>(modes.size(), static_cast<std::size_t>(max_modes_)));
        const double fs = sample_rate_;
        constexpr double two_pi = 6.28318530717958647692;
        for (int m = 0; m < count; ++m) {
            const auto i = static_cast<std::size_t>(m);
            const double freq = std::clamp(static_cast<double>(modes[i].freq_hz), 1.0, 0.49 * fs);
            const double t60 = std::max(static_cast<double>(modes[i].t60_s), 1.0e-3);
            const double r = std::pow(10.0, -3.0 / (t60 * fs));
            const double w = two_pi * freq / fs;
            const double cos_w = std::cos(w);
            const double sin_w = std::sin(w);
            const double gain = static_cast<double>(modes[i].gain);
            rc_[i] = static_cast<float>(r * cos_w);
            rs_[i] = static_cast<float>(r * sin_w);
            cu_[i] = static_cast<float>(gain * cos_w);
            cv_[i] = static_cast<float>(gain * sin_w);
            w_out_[i] = i < out_weights.size() ? out_weights[i] : 1.0f;
            freq_[i] = static_cast<float>(freq);
            omega_[i] = static_cast<float>(two_pi * freq);  // rad/s, physical
            r_[i] = static_cast<float>(r);
            r_base_[i] = static_cast<float>(r);       // authored decay, for Decay control
            gain_[i] = static_cast<float>(gain);
            freq_base_[i] = static_cast<float>(freq);  // authored pitch, for retune
        }
        for (std::size_t m = static_cast<std::size_t>(count); m < padded_; ++m) {
            cu_[m] = cv_[m] = rc_[m] = rs_[m] = 0.0f;
            w_out_[m] = 0.0f;
            freq_[m] = 0.0f;
            omega_[m] = 0.0f;
            r_[m] = r_base_[m] = gain_[m] = freq_base_[m] = 0.0f;
        }
        mode_count_ = count;
        rebuild_triads();
    }

    /// Retune every mode by a uniform frequency multiplier, relative to the
    /// authored spectrum (the Tune control and per-note pitch). Alloc- and
    /// lock-free: the triad set is NOT rebuilt because the sum-resonance
    /// condition omega_h = omega_l + omega_m is invariant under a uniform scale,
    /// so the precomputed triads stay valid. Resonator state (u,v) is preserved,
    /// so retuning a ringing plate changes its pitch without stepping amplitude
    /// (the coupled-form amplitude-safe property). This is the RT-safe note-on /
    /// Tune path; use set_modes() only to change the mode set itself.
    void retune(double ratio) noexcept {
        if (ratio <= 0.0) return;
        const double fs = sample_rate_;
        constexpr double two_pi = 6.28318530717958647692;
        for (int m = 0; m < mode_count_; ++m) {
            const auto i = static_cast<std::size_t>(m);
            const double freq = std::clamp(static_cast<double>(freq_base_[i]) * ratio, 1.0, 0.49 * fs);
            const double w = two_pi * freq / fs;
            const double cos_w = std::cos(w);
            const double sin_w = std::sin(w);
            const double r = r_[i], gain = gain_[i];
            rc_[i] = static_cast<float>(r * cos_w);
            rs_[i] = static_cast<float>(r * sin_w);
            cu_[i] = static_cast<float>(gain * cos_w);
            cv_[i] = static_cast<float>(gain * sin_w);
            freq_[i] = static_cast<float>(freq);
            omega_[i] = static_cast<float>(two_pi * freq);
        }
    }

    /// Scale every mode's decay time by @p factor (the Decay control), relative
    /// to the authored T60. Alloc-free; recomputes the pole radius per mode.
    void set_decay_scale(double factor) noexcept {
        if (factor <= 0.0) return;
        const double fs = sample_rate_;
        for (int m = 0; m < mode_count_; ++m) {
            const auto i = static_cast<std::size_t>(m);
            // r = 10^(-3/(t60*fs)); scaling t60 by `factor` -> r^(1/factor).
            const double r = std::pow(static_cast<double>(r_base_[i]), 1.0 / factor);
            r_[i] = static_cast<float>(r);
            // Rewrite rc/rs at the current pitch.
            const double w = static_cast<double>(omega_[i]) / fs;
            rc_[i] = static_cast<float>(r * std::cos(w));
            rs_[i] = static_cast<float>(r * std::sin(w));
        }
    }

    /// Set the bloom (coupling) scale in [0, 1]: 0 disables the nonlinearity
    /// entirely (a pure linear inharmonic cloud that matches ModalBankT
    /// mode-for-mode); 1 is the full tam-tam cascade. Control-rate, no alloc.
    void set_bloom(double bloom) noexcept { bloom_ = std::clamp(bloom, 0.0, 1.0); }
    double bloom() const noexcept { return bloom_; }

    /// Overall coupling rate constant (absorbs the arbitrary phasor-energy
    /// units into a single tuned gain). The transfer per triad per block is
    /// eta * kappa * K_shape * (n_l n_m - ...) * dt, so kappa sets how fast a
    /// given-amplitude strike blooms. An ABSOLUTE scale is required, not a
    /// relative one: the amplitude dependence (soft barely blooms, hard blooms
    /// hard) lives precisely in comparing n_l*n_m against a fixed rate, so this
    /// must NOT be normalized away by the signal level. Tuned for the
    /// velocity-normalized strike amplitudes the instrument feeds.
    void set_coupling_scale(double kappa) noexcept { coupling_scale_ = std::max(kappa, 0.0); }
    double coupling_scale() const noexcept { return coupling_scale_; }

    /// Advance @p count samples of the (already externally injected) input into
    /// @p out (accumulating), running the linear ring at sample rate and the
    /// bloom coupling at block rate. @p input is per-sample excitation (a strike
    /// pulse or zeros to ring out). Allocation- and lock-free.
    void render(const float* input, float* out, int count) {
        if (count <= 0 || mode_count_ <= 0) return;
        int pos = 0;
        while (pos < count) {
            const int chunk = std::min(kCouplingBlock, count - pos);
            ring(input + pos, out + pos, chunk);
            if (bloom_ > 0.0 && triad_count_ > 0)
                couple(static_cast<double>(chunk) / sample_rate_);
            pos += chunk;
        }
    }

    /// Zero all resonator state (discontinuity reset). Modes and triads persist.
    void reset() {
        std::fill(u_.begin(), u_.end(), 0.0f);
        std::fill(v_.begin(), v_.end(), 0.0f);
    }

    /// Total internal modal energy proxy (sum of squared phasor magnitudes).
    /// The quantity the boundedness invariant tracks; exposed for tests.
    double total_energy() const noexcept {
        double e = 0.0;
        for (int m = 0; m < mode_count_; ++m) {
            const auto i = static_cast<std::size_t>(m);
            const double u = u_[i], v = v_[i];
            e += u * u + v * v;
        }
        return e;
    }

    int mode_count() const noexcept { return mode_count_; }
    int triad_count() const noexcept { return triad_count_; }
    double sample_rate() const noexcept { return sample_rate_; }

private:
    // Linear ring: ModalBankT's coupled-form loop, one pickup with a per-mode
    // output weight. Kept lane-grouped so it auto-vectorizes; the per-mode u_/v_
    // state is left in flat SoA so the coupling pass can read/write it.
    void ring(const float* input, float* out, int num_samples) {
        const int groups = (mode_count_ + kLanes - 1) / kLanes;
        for (int g = 0; g < groups; ++g) {
            const std::size_t base = static_cast<std::size_t>(g) * kLanes;
            float cu[kLanes], cv[kLanes], rc[kLanes], rs[kLanes];
            float u[kLanes], v[kLanes], wo[kLanes];
            for (int l = 0; l < kLanes; ++l) {
                const std::size_t i = base + static_cast<std::size_t>(l);
                cu[l] = cu_[i]; cv[l] = cv_[i]; rc[l] = rc_[i]; rs[l] = rs_[i];
                u[l] = u_[i]; v[l] = v_[i]; wo[l] = w_out_[i];
            }
            for (int i = 0; i < num_samples; ++i) {
                const float x = input[i];
                float y[kLanes];
                for (int l = 0; l < kLanes; ++l) {
                    const float nu = rc[l] * u[l] - rs[l] * v[l] + cu[l] * x;
                    const float nv = rs[l] * u[l] + rc[l] * v[l] + cv[l] * x;
                    u[l] = nu;
                    v[l] = nv;
                    y[l] = wo[l] * nv;
                }
                for (int step = kLanes / 2; step > 0; step /= 2)
                    for (int l = 0; l < step; ++l) y[l] += y[l + step];
                out[i] += y[0];
            }
            for (int l = 0; l < kLanes; ++l) {
                const std::size_t i = base + static_cast<std::size_t>(l);
                u_[i] = signal::snap_to_zero(u[l]);
                v_[i] = signal::snap_to_zero(v[l]);
            }
        }
    }

    // One block-rate three-wave kinetic energy-transfer pass over the triad set.
    //
    // Per triad (l, m, h) with omega_h ~ omega_l + omega_m, in the action
    // n_p = E_p / omega_p (E_p the phasor energy u^2+v^2):
    //
    //   F  = eta * K * ( n_l*n_m  -  n_h*(n_l+n_m) ) * dt      (Manley-Rowe flow)
    //
    // is the flow of action from the low pair to the high mode. F>0 up-cascades
    // (the bloom); it is fourth-order in strike amplitude (F ~ n_l*n_m ~ a^4), so
    // a soft strike barely transfers and a hard strike transfers hard -- the
    // measurable amplitude dependence. Energy is moved EXACTLY conservatively:
    // de_l = omega_l*F and de_m = omega_m*F leave the pair and their sum enters
    // h, bit-for-bit, so the pass neither creates nor destroys energy. F is
    // clamped so every energy stays >= 0 (donors not over-drained, receiver not
    // driven negative), which -- with the strictly-dissipative linear T60 --
    // makes total energy monotone non-increasing. Bounded for any hardness.
    void couple(double dt) {
        const double eta = bloom_;
        for (int t = 0; t < triad_count_; ++t) {
            const std::size_t il = static_cast<std::size_t>(tl_[static_cast<std::size_t>(t)]);
            const std::size_t im = static_cast<std::size_t>(tm_[static_cast<std::size_t>(t)]);
            const std::size_t ih = static_cast<std::size_t>(th_[static_cast<std::size_t>(t)]);

            // The three modes of a triad must be distinct: the energy books
            // apply a delta to each index once, so an aliased index would not
            // conserve energy. The enumeration guarantees this (donor pair l<m,
            // receiver h != l,m); guard defensively regardless.
            if (il == im || il == ih || im == ih) continue;

            double el = static_cast<double>(u_[il]) * u_[il] + static_cast<double>(v_[il]) * v_[il];
            double em = static_cast<double>(u_[im]) * u_[im] + static_cast<double>(v_[im]) * v_[im];
            double eh = static_cast<double>(u_[ih]) * u_[ih] + static_cast<double>(v_[ih]) * v_[ih];

            const double wl = omega_[il], wm = omega_[im], wh = omega_[ih];
            if (wl <= 0.0 || wm <= 0.0 || wh <= 0.0) continue;

            const double nl = el / wl, nm = em / wm, nh = eh / wh;
            const double k = eta * coupling_scale_
                           * static_cast<double>(tk_[static_cast<std::size_t>(t)]) * dt;
            double f = k * (nl * nm - nh * (nl + nm));

            // Clamp so no energy goes negative: donors E_l -= wl*F, E_m -= wm*F
            // stay >= 0  ->  F <= min(nl, nm); receiver E_h += (wl+wm)*F stays
            // >= 0  ->  F >= -eh/(wl+wm).
            const double f_max = std::min(nl, nm);
            const double f_min = -eh / (wl + wm);
            f = std::clamp(f, f_min, f_max);
            if (f == 0.0) continue;

            const double de_l = wl * f;
            const double de_m = wm * f;
            el -= de_l;
            em -= de_m;
            eh += de_l + de_m;                 // exact conservation: what left enters h

            apply_energy(il, el);
            apply_energy(im, em);
            apply_energy(ih, eh);
        }
    }

    // Rescale mode i's phasor to carry target energy e_new (>= 0). Scaling both
    // state components preserves phase and frequency exactly (the coupled-form
    // amplitude-safe property). A cold receiver that is gaining is seeded at the
    // target amplitude with an arbitrary phase -- an inaudible one-block
    // transient, and rare because struck modes are never exactly zero.
    void apply_energy(std::size_t i, double e_new) {
        e_new = std::max(e_new, 0.0);
        const double u = u_[i], v = v_[i];
        const double e_old = u * u + v * v;
        if (e_old > 1.0e-30) {
            const float g = static_cast<float>(std::sqrt(e_new / e_old));
            u_[i] = static_cast<float>(u) * g;
            v_[i] = static_cast<float>(v) * g;
        } else if (e_new > 0.0) {
            v_[i] = static_cast<float>(std::sqrt(e_new));
            u_[i] = 0.0f;
        }
    }

    // Enumerate near-resonant sum triads omega_h ~ omega_l + omega_m within a
    // cents tolerance, weight each by closeness of resonance, then cap the
    // number of triads each mode participates in (highest weight first). The
    // cap pins the kept set to K = O(mode_count) with a hard ceiling of
    // mode_count*max_partners -> deterministic RT cost. Runs only on a mode-set
    // change (the sum-resonance condition is scale-invariant, so Tune never
    // rebuilds it). Allocation is permitted here.
    void rebuild_triads() {
        triad_count_ = 0;
        std::fill(part_count_.begin(), part_count_.end(), 0);
        if (max_partners_ == 0 || mode_count_ < 3) return;

        const int mc = mode_count_;
        constexpr double tol_cents = 30.0;
        const double tol = tol_cents / 1200.0;  // |ln(f_h/target)| <= tol*ln2

        // Sorted (freq, index) for a binary-searched target band.
        std::vector<std::pair<float, int>> order(static_cast<std::size_t>(mc));
        for (int i = 0; i < mc; ++i)
            order[static_cast<std::size_t>(i)] = {freq_[static_cast<std::size_t>(i)], i};
        std::sort(order.begin(), order.end());

        struct Cand { float k; int l, m, h; };
        std::vector<Cand> cands;
        cands.reserve(static_cast<std::size_t>(mc) * 8);

        const double band = std::exp(tol * 0.69314718055994530942);  // exp(tol*ln2)
        for (int a = 0; a < mc; ++a) {
            const double fa = freq_[static_cast<std::size_t>(a)];
            // Distinct donor pair only (b>a): a self-pair (l==m, the 2*omega_l
            // second-harmonic channel) would make the two donor slots alias one
            // mode, and the per-mode energy application cannot conserve energy
            // for an aliased donor. Excluding it keeps the transfer provably
            // energy-conserving; the up-cascade runs through the distinct pairs.
            for (int b = a + 1; b < mc; ++b) {
                const double target = fa + freq_[static_cast<std::size_t>(b)];
                if (target >= 0.49 * sample_rate_) break;  // no receiver above Nyquist
                const float lo = static_cast<float>(target / band);
                const float hi = static_cast<float>(target * band);
                auto it = std::lower_bound(order.begin(), order.end(),
                                           std::make_pair(lo, -1));
                for (; it != order.end() && it->first <= hi; ++it) {
                    const int h = it->second;
                    if (h == a || h == b) continue;
                    const double fh = it->first;
                    const double detune_cents =
                        1200.0 * std::log2(fh / target);
                    // Weight: strongest at exact resonance, falling with detuning.
                    const float k = static_cast<float>(1.0 / (1.0 + std::abs(detune_cents)));
                    cands.push_back({k, a, b, h});
                }
            }
        }

        std::sort(cands.begin(), cands.end(),
                  [](const Cand& x, const Cand& y) { return x.k > y.k; });

        const std::size_t ceiling = tl_.size() ? tl_.size() - 1 : 0;
        for (const auto& c : cands) {
            if (static_cast<std::size_t>(triad_count_) >= ceiling) break;
            auto& pl = part_count_[static_cast<std::size_t>(c.l)];
            auto& pm = part_count_[static_cast<std::size_t>(c.m)];
            auto& ph = part_count_[static_cast<std::size_t>(c.h)];
            if (pl >= max_partners_ || pm >= max_partners_ || ph >= max_partners_)
                continue;
            const auto t = static_cast<std::size_t>(triad_count_);
            tl_[t] = c.l; tm_[t] = c.m; th_[t] = c.h; tk_[t] = c.k;
            ++pl; ++pm; ++ph;
            ++triad_count_;
        }
    }

    signal::AlignedBufferT<float> cu_, cv_, rc_, rs_, u_, v_, w_out_, freq_, omega_;
    signal::AlignedBufferT<float> r_, r_base_, gain_, freq_base_;  // for retune / Decay
    std::vector<int> tl_, tm_, th_;   // triad mode indices (SoA)
    std::vector<float> tk_;           // triad coupling weights
    std::vector<int> part_count_;     // per-mode participation, for the cap

    double sample_rate_ = 48000.0;
    double bloom_ = 0.0;
    double coupling_scale_ = 1.0e5;   ///< tuned default (see set_coupling_scale)
    std::size_t padded_ = 0;
    int max_modes_ = 0;
    int max_partners_ = 16;
    int mode_count_ = 0;
    int triad_count_ = 0;
};

// ── Procedural thin-plate spectrum generator ────────────────────────────────

/// A generated gong/tam-tam mode set: the inharmonic cloud plus the per-mode
/// output weights of a slightly-off-center strike.
struct PlateSpectrum {
    std::vector<signal::ModalMode> modes;
    std::vector<float> out_weights;
    double max_t60_s = 0.0;
};

/// Generate a dense inharmonic thin-plate (Kirchhoff free circular plate) mode
/// set anchored so the fundamental (2,0) mode sits at @p f0. Bending waves are
/// dispersive (f ~ beta^2), so the modes spread inharmonic. Low-mode ratios are
/// the classic free-plate values (Leissa; Fletcher & Rossing, The Physics of
/// Musical Instruments); higher modes use the Kirchhoff asymptotic
/// beta_mn ~ pi(n + m/2 + c0), giving f ~ beta^2 -- the quadratic dispersive
/// spread. T60 is long at low modes and short at high (radiation + internal
/// damping rise with f), producing the long shimmering tail. Output weights come
/// from a slightly-off-center strike with a finite-width mallet, so the whole
/// cloud lights up (a dead-center strike would excite only axisymmetric modes).
inline PlateSpectrum generate_plate_spectrum(double f0, double f_max = 12000.0,
                                             int max_modes = 1024) {
    PlateSpectrum s;
    if (f0 <= 0.0) return s;

    // beta^2 ratios relative to the (2,0) fundamental. Low modes: measured
    // free-plate values. Anchor (2,0)=1.
    // (m nodal diameters, n nodal circles) -> f/f0 ratio.
    struct Low { int m, n; double ratio; };
    static constexpr Low low[] = {
        {2, 0, 1.00}, {0, 1, 1.73}, {3, 0, 2.33}, {1, 1, 3.91},
        {4, 0, 4.11}, {2, 1, 6.71}, {5, 0, 6.30}, {0, 2, 7.34},
        {3, 1, 9.38}, {6, 0, 8.72}, {1, 2, 11.4}, {4, 1, 12.4},
    };

    struct Gen { double ratio; int m, n; };
    std::vector<Gen> gens;
    for (const auto& lo : low) gens.push_back({lo.ratio, lo.m, lo.n});

    // Higher modes: Kirchhoff asymptotic beta ~ pi(n + m/2 + c0), f ~ beta^2.
    // Calibrated so beta(2,0) reproduces ratio 1 at c0.
    constexpr double c0 = 0.842;  // free-plate edge correction (approx)
    const double beta20 = (0.0 + 2.0 / 2.0 + c0);  // n=0, m=2
    for (int mm = 0; mm <= 24; ++mm) {
        for (int nn = 0; nn <= 12; ++nn) {
            // Skip the low modes already listed (avoid duplicates in that band).
            bool dup = false;
            for (const auto& lo : low)
                if (lo.m == mm && lo.n == nn) { dup = true; break; }
            if (dup) continue;
            const double beta = (nn + mm / 2.0 + c0);
            const double ratio = (beta * beta) / (beta20 * beta20);
            if (ratio < 1.0) continue;  // below the fundamental: skip
            gens.push_back({ratio, mm, nn});
        }
    }

    std::sort(gens.begin(), gens.end(),
              [](const Gen& a, const Gen& b) { return a.ratio < b.ratio; });

    const double t60_0 = 14.0;      // fundamental sustain (s) -- tam-tams ring long
    const double f_c = 2200.0;
                                    // cascade-populated shimmer can sustain)
    const double p = 1.0;           // damping rolloff exponent
    // Strike/mallet rolloff: a real mallet's finite contact deposits the strike
    // energy predominantly in the LOW modes, so the attack is dark and the
    // nonlinear up-cascade (not the strike) is what brightens the spectrum over
    // the following seconds -- the audible "bloom builds after the strike".
    const double f_roll = 900.0;

    for (const auto& gnr : gens) {
        const double f = f0 * gnr.ratio;
        if (f > f_max) continue;
        if (static_cast<int>(s.modes.size()) >= max_modes) break;

        const double t60 = t60_0 / (1.0 + std::pow(f / f_c, p));

        // Slightly-off-center strike: axisymmetric (m=0) modes strongest, m>=1
        // modes partially excited (a real strike is not perfectly centered), all
        // rolled off at high f by the finite mallet contact. Output weight folds
        // strike * pickup shape into one readout scalar.
        const double axis = gnr.m == 0 ? 1.0 : 0.55 / (1.0 + 0.12 * gnr.m);
        const double mallet = 1.0 / (1.0 + std::pow(f / f_roll, 1.6));
        const double gain = axis * mallet;

        signal::ModalMode mode;
        mode.freq_hz = static_cast<float>(f);
        mode.t60_s = static_cast<float>(t60);
        mode.gain = static_cast<float>(gain);
        s.modes.push_back(mode);
        s.out_weights.push_back(1.0f);
        s.max_t60_s = std::max(s.max_t60_s, t60);
    }
    return s;
}

}  // namespace pulp::examples
