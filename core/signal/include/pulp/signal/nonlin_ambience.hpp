#pragma once

/// @file nonlin_ambience.hpp
/// Runtime nonlinear ambience processor. Program constants and documented
/// topology live in nonlin_ambience_design.hpp.

#include <pulp/signal/dither.hpp>
#include <pulp/signal/detail/schroeder_allpass.hpp>
#include <pulp/signal/nonlin_ambience_design.hpp>

namespace pulp::signal {

/// The nonlin / gated ambience engine. Stereo, processed in place.
template <typename SampleType = float>
class NonlinAmbienceT {
public:
    /// Maximum tap-builder iterations charged to one audio frame. Topology
    /// automation is staged into the inactive bank and therefore has a hard,
    /// input-independent callback cost instead of rebuilding the room at once.
    static constexpr int kTopologyWorkPerSample = 4;
    /// One velvet tap: where to read, how much to scale it, and which spectral
    /// segment it belongs to. `segment` is precomputed at rebuild so the audio
    /// path never divides.
    struct Tap {
        int delay = 0;
        float gain = 0.0f;
        int segment = 0;
    };

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Allocates the tap ring and both tap banks. The only function here that
    /// may allocate. `max_length_ms` and `max_predelay_ms` size the storage; no
    /// later `set_*` can outgrow them because both are clamped to these maxima.
    void prepare(double sample_rate,
                 double max_length_ms = nonlin_ambience::kMaxLengthMs,
                 double max_predelay_ms = nonlin_ambience::kMaxPredelayMs) {
        namespace na = nonlin_ambience;

        sample_rate_ = (std::isfinite(sample_rate) && sample_rate > 0.0) ? sample_rate
                                                                         : 48000.0;
        max_length_ms_ = std::isfinite(max_length_ms)
                             ? std::max(nonlin_ambience::kMinLengthMs, max_length_ms)
                             : nonlin_ambience::kMaxLengthMs;
        max_predelay_ms_ = std::isfinite(max_predelay_ms) ? std::max(0.0, max_predelay_ms)
                                                          : nonlin_ambience::kMaxPredelayMs;

        const int max_len_samples = static_cast<int>(std::ceil(
            units::ms_to_samples(max_length_ms_, sample_rate_)));
        const int max_pre_samples = static_cast<int>(std::ceil(
            units::ms_to_samples(max_predelay_ms_, sample_rate_)));

        // Power-of-two ring: the tap read is the hot loop and a mask is one
        // instruction where a modulo is a division. See the file note.
        ring_mask_ = static_cast<int>(next_pow2(static_cast<std::size_t>(
                         max_len_samples + max_pre_samples + 2))) - 1;
        ring_.assign(static_cast<std::size_t>(ring_mask_) + 1u, SampleType{0});
        ring_write_ = 0;

        // A grid step never advances less than fs/Nd_max samples, so the number
        // of steps inside an L-sample window is at most L·Nd_max/fs — which is
        // (max_length_ms/1000)·Nd_max, independent of the sample rate.
        tap_capacity_ = static_cast<int>(std::ceil(
                            na::kNdMax * max_length_ms_ / 1000.0)) + na::kTapGuard;
        for (int bank = 0; bank < 2; ++bank)
            for (int ch = 0; ch < 2; ++ch) {
                taps_[bank][ch].assign(static_cast<std::size_t>(tap_capacity_), Tap{});
                tap_count_[bank][ch] = 0;
            }

        for (int i = 0; i < na::kNumAllpass; ++i) {
            allpass_len_[i] = nearest_prime(std::max(2, static_cast<int>(std::lround(
                units::ms_to_samples(na::kAllpassDelaysMs[i], sample_rate_)))));
            allpass_[i].prepare(allpass_len_[i] + 1);
        }

        swap_fade_len_ = std::max(1, static_cast<int>(std::lround(
            units::ms_to_samples(na::kSwapFadeMs, sample_rate_))));

        const auto smooth_s = static_cast<SampleType>(na::kParamSmoothMs / 1000.0);
        const auto fs = static_cast<SampleType>(sample_rate_);
        mix_.set_ramp_time(smooth_s, fs);
        width_.set_ramp_time(smooth_s, fs);
        output_gain_.set_ramp_time(smooth_s, fs);
        converter_.set_ramp_time(smooth_s, fs);

        for (int ch = 0; ch < 2; ++ch) {
            conv_hp_[ch].prepare(static_cast<SampleType>(sample_rate_));
            conv_hp_[ch].set_cutoff(static_cast<SampleType>(na::kConverterFcLo));
            conv_lp_[ch].prepare(static_cast<SampleType>(sample_rate_));
            conv_lp_[ch].set_cutoff(static_cast<SampleType>(na::kConverterFcHi));
            conv_dc_[ch].set_pole(static_cast<SampleType>(na::kConverterDcPole));
        }

        update_segment_coefficients();
        rebuild_immediate();
        reset();
    }

    /// Clears every state. Never allocates. Tap tables survive — they are a pure
    /// function of the parameters, not of the signal history.
    void reset() {
        std::fill(ring_.begin(), ring_.end(), SampleType{0});
        ring_write_ = 0;
        for (int i = 0; i < nonlin_ambience::kNumAllpass; ++i) allpass_[i].reset();
        for (auto& bank : seg_state_)
            for (auto& ch : bank) ch.fill(SampleType{0});
        for (int ch = 0; ch < 2; ++ch) {
            conv_hp_[ch].reset();
            conv_lp_[ch].reset();
            conv_dc_[ch].reset();
        }
        dither_.reset();
        mix_.set_immediate(mix_.target());
        width_.set_immediate(width_.target());
        output_gain_.set_immediate(output_gain_.target());
        converter_.set_immediate(converter_.target());
        // A pending swap is resolved rather than carried across a reset: the
        // crossfade is a signal-domain artifact and there is no signal now.
        if (fading_) {
            front_ = pending_;
            fading_ = false;
        }
        fade_pos_ = 0;
    }

    /// Series law 5: exactly zero. The dry path is a straight wire and the wet
    /// pre-delay is the effect, not latency.
    static constexpr int latency_samples() { return 0; }

    // ── Topology parameters (rebuild the FIR) ─────────────────────────────

    void set_program(NonlinProgram program) {
        if (program == program_) return;
        program_ = program;
        rebuild_and_swap();
    }

    /// Window length in ms — the absolute time the dimensionless envelope is
    /// stretched across. Clamped to the `prepare()` maximum.
    void set_length_ms(double ms) {
        if (!std::isfinite(ms)) return;
        const double v = std::clamp(ms, nonlin_ambience::kMinLengthMs, max_length_ms_);
        if (v == length_ms_) return;
        length_ms_ = v;
        rebuild_and_swap();
    }

    void set_predelay_ms(double ms) {
        if (!std::isfinite(ms)) return;
        const double v = std::clamp(ms, 0.0, max_predelay_ms_);
        if (v == predelay_ms_) return;
        predelay_ms_ = v;
        rebuild_and_swap();
    }

    /// Scales `Nd_min` only; `Nd_max` is fixed so the late field stays above the
    /// smoothness floor no matter how sparse the early field is made.
    void set_density_pct(double pct) {
        if (!std::isfinite(pct)) return;
        const double v = std::clamp(pct, nonlin_ambience::kMinDensityPct, 100.0);
        if (v == density_pct_) return;
        density_pct_ = v;
        rebuild_and_swap();
    }

    void set_density_growth(double gamma) {
        if (!std::isfinite(gamma)) return;
        const double v = std::clamp(gamma, 0.0, 2.0);
        if (v == density_growth_) return;
        density_growth_ = v;
        rebuild_and_swap();
    }

    /// The Gated program's hold point, as a percent of the window.
    void set_gate_hold_pct(double pct) {
        if (!std::isfinite(pct)) return;
        const double v = std::clamp(pct, 10.0, 95.0);
        if (v == gate_hold_pct_) return;
        gate_hold_pct_ = v;
        rebuild_and_swap();
    }

    /// The Reverse program's rise point and the NonLin2 program's gate point,
    /// as a percent of the window. Ignored by Ambience and Gated.
    void set_attack_pct(double pct) {
        if (!std::isfinite(pct)) return;
        const double v = std::clamp(pct, 5.0, 98.0);
        if (v == attack_pct_) return;
        attack_pct_ = v;
        rebuild_and_swap();
    }

    /// Atomically applies the complete topology snapshot and regenerates the
    /// inactive tap bank at most once. Catalog/host block boundaries use this
    /// instead of invoking seven independently rebuilding setters.
    void set_topology(NonlinProgram program, double length_ms, double predelay_ms,
                      double density_pct, double density_growth,
                      double gate_hold_pct, double attack_pct) {
        const double length = std::isfinite(length_ms)
                                  ? std::clamp(length_ms, nonlin_ambience::kMinLengthMs,
                                               max_length_ms_)
                                  : length_ms_;
        const double predelay = std::isfinite(predelay_ms)
                                    ? std::clamp(predelay_ms, 0.0, max_predelay_ms_)
                                    : predelay_ms_;
        const double density = std::isfinite(density_pct)
                                   ? std::clamp(density_pct,
                                                nonlin_ambience::kMinDensityPct, 100.0)
                                   : density_pct_;
        const double growth = std::isfinite(density_growth)
                                  ? std::clamp(density_growth, 0.0, 2.0)
                                  : density_growth_;
        const double hold = std::isfinite(gate_hold_pct)
                                ? std::clamp(gate_hold_pct, 10.0, 95.0)
                                : gate_hold_pct_;
        const double attack = std::isfinite(attack_pct)
                                  ? std::clamp(attack_pct, 5.0, 98.0)
                                  : attack_pct_;
        if (program == program_ && length == length_ms_ && predelay == predelay_ms_ &&
            density == density_pct_ && growth == density_growth_ &&
            hold == gate_hold_pct_ && attack == attack_pct_)
            return;
        program_ = program;
        length_ms_ = length;
        predelay_ms_ = predelay;
        density_pct_ = density;
        density_growth_ = growth;
        gate_hold_pct_ = hold;
        attack_pct_ = attack;
        rebuild_and_swap();
    }

    /// RT-bounded counterpart used by hosted automation. The requested
    /// snapshot is generated incrementally into the inactive bank; the audible
    /// topology changes only after the complete, normalized bank is ready.
    void request_topology(NonlinProgram program, double length_ms, double predelay_ms,
                          double density_pct, double density_growth,
                          double gate_hold_pct, double attack_pct) {
        const double length = std::isfinite(length_ms)
                                  ? std::clamp(length_ms, nonlin_ambience::kMinLengthMs,
                                               max_length_ms_)
                                  : length_ms_;
        const double predelay = std::isfinite(predelay_ms)
                                    ? std::clamp(predelay_ms, 0.0, max_predelay_ms_)
                                    : predelay_ms_;
        const double density = std::isfinite(density_pct)
                                   ? std::clamp(density_pct,
                                                nonlin_ambience::kMinDensityPct, 100.0)
                                   : density_pct_;
        const double growth = std::isfinite(density_growth)
                                  ? std::clamp(density_growth, 0.0, 2.0)
                                  : density_growth_;
        const double hold = std::isfinite(gate_hold_pct)
                                ? std::clamp(gate_hold_pct, 10.0, 95.0)
                                : gate_hold_pct_;
        const double attack = std::isfinite(attack_pct)
                                  ? std::clamp(attack_pct, 5.0, 98.0)
                                  : attack_pct_;
        if (program == program_ && length == length_ms_ && predelay == predelay_ms_ &&
            density == density_pct_ && growth == density_growth_ &&
            hold == gate_hold_pct_ && attack == attack_pct_)
            return;
        program_ = program;
        length_ms_ = length;
        predelay_ms_ = predelay;
        density_pct_ = density;
        density_growth_ = growth;
        gate_hold_pct_ = hold;
        attack_pct_ = attack;
        topology_dirty_ = true;
        topology_build_.active = false;  // restart from the newest atomic snapshot
    }

    std::uint64_t topology_rebuild_count() const { return topology_rebuild_count_; }
    int topology_work_units_last_sample() const { return topology_work_units_last_sample_; }

    /// Seed for the L channel; R derives as `seed ⊕ kSeedRodd`. Series law 2 —
    /// a preset choice, never automated.
    void set_seed(std::uint32_t seed) {
        if (seed == seed_) return;
        seed_ = seed;
        rebuild_and_swap();
    }

    /// Allpass coefficient. Touches two scalars and no tap, so it does not
    /// rebuild (see the file note). Clamped below 1 for stability.
    void set_diffusion(double g) {
        if (!std::isfinite(g)) return;
        diffusion_ = std::clamp(g, 0.0, nonlin_ambience::kDiffusionMax);
    }

    // ── Continuous parameters (smoothed, never rebuild) ───────────────────

    /// Spectral tilt `T ∈ [−1, +1]`: positive keeps highs later (brighter),
    /// negative darkens. Recomputes the segment coefficients rather than
    /// smoothing them — a one-pole corner change is state-continuous, so it
    /// produces no step in the output the way a gain change would.
    void set_tone(double tone) {
        if (!std::isfinite(tone)) return;
        const double v = std::clamp(tone, -1.0, 1.0);
        if (v == tone_) return;
        tone_ = v;
        update_segment_coefficients();
    }

    void set_hf_damp_hz(double hz) {
        if (!std::isfinite(hz)) return;
        const double v = std::clamp(hz, 1000.0, 18000.0);
        if (v == hf_damp_hz_) return;
        hf_damp_hz_ = v;
        update_segment_coefficients();
    }

    /// 0 % is mono (both channels exactly the mid signal), 100 % is the two
    /// independent velvet realizations.
    void set_width_pct(double pct) {
        if (!std::isfinite(pct)) return;
        width_.set_target(static_cast<SampleType>(std::clamp(pct, 0.0, 100.0) / 100.0));
    }

    /// 0 disables the converter stage entirely.
    void set_converter_amount(double amount) {
        if (!std::isfinite(amount)) return;
        converter_.set_target(static_cast<SampleType>(std::clamp(amount, 0.0, 1.0)));
    }

    void set_output_gain_db(double db) {
        if (!std::isfinite(db)) return;
        output_gain_.set_target(
            units::db_to_linear(static_cast<SampleType>(std::clamp(db, -24.0, 24.0))));
    }

    /// Dry/wet, 0 .. 100 %. The default is 100 (send style).
    void set_mix_pct(double pct) {
        if (!std::isfinite(pct)) return;
        mix_.set_target(static_cast<SampleType>(std::clamp(pct, 0.0, 100.0) / 100.0));
    }

    // ── Processing ────────────────────────────────────────────────────────

    /// One stereo frame, in place. Allocation-free, lock-free, branch-light.
    void process_sample(SampleType& left, SampleType& right) {
        if (!std::isfinite(static_cast<double>(left)) ||
            !std::isfinite(static_cast<double>(right))) {
            left = SampleType{0};
            right = SampleType{0};
            return;
        }
        const SampleType dry_l = left;
        const SampleType dry_r = right;

        // Mono sum into the diffuser: the tap cloud is one shared field.
        SampleType d = SampleType{0.5} * (dry_l + dry_r);
        const auto g = static_cast<SampleType>(diffusion_);
        for (int i = 0; i < nonlin_ambience::kNumAllpass; ++i) {
            const SampleType delayed = allpass_[i].read(allpass_len_[i] - 1);
            const SampleType write =
                snap_to_zero(detail::schroeder_allpass_write(d, delayed, g));
            d = detail::schroeder_allpass_output(delayed, write, g);
            allpass_[i].push(write);
        }

        ring_write_ = (ring_write_ + 1) & ring_mask_;
        ring_[static_cast<std::size_t>(ring_write_)] = d;

        SampleType wet_l = SampleType{0};
        SampleType wet_r = SampleType{0};

        if (!fading_) {
            wet_l = render_bank(front_, 0);
            wet_r = render_bank(front_, 1);
        } else {
            SampleType old_gain{}, new_gain{};
            const auto u = static_cast<SampleType>(fade_pos_) /
                           static_cast<SampleType>(swap_fade_len_);
            crossfade_gains(crossfade_smoothstep(u), CrossfadeGainLaw::EqualPower,
                            old_gain, new_gain);
            wet_l = old_gain * render_bank(front_, 0) + new_gain * render_bank(pending_, 0);
            wet_r = old_gain * render_bank(front_, 1) + new_gain * render_bank(pending_, 1);
            if (++fade_pos_ >= swap_fade_len_) {
                // The retiring bank's tilt filters are cleared so a later swap
                // back into it does not start from stale state.
                seg_state_[front_][0].fill(SampleType{0});
                seg_state_[front_][1].fill(SampleType{0});
                front_ = pending_;
                fading_ = false;
                fade_pos_ = 0;
            }
        }

        const SampleType conv = converter_.next();
        if (conv > SampleType{0}) {
            wet_l = converter_stage(0, wet_l, conv);
            wet_r = converter_stage(1, wet_r, conv);
        }

        // Width: mid/side. Exactly mono at 0, exactly the two realizations at 1.
        const SampleType w = width_.next();
        const SampleType mid = SampleType{0.5} * (wet_l + wet_r);
        wet_l = mid + w * (wet_l - mid);
        wet_r = mid + w * (wet_r - mid);

        const SampleType trim = output_gain_.next();
        wet_l *= trim;
        wet_r *= trim;

        SampleType dry_gain{}, wet_gain{};
        crossfade_gains(mix_.next(), CrossfadeGainLaw::EqualGain, dry_gain, wet_gain);
        left = dry_gain * dry_l + wet_gain * wet_l;
        right = dry_gain * dry_r + wet_gain * wet_r;
        advance_topology_rebuild();
    }

    /// A block of stereo frames, in place.
    void process(SampleType* left, SampleType* right, int num_samples) {
        for (int n = 0; n < num_samples; ++n) process_sample(left[n], right[n]);
    }

    // ── Introspection ─────────────────────────────────────────────────────
    //
    // The tap table IS the product of this module, so it is readable. The
    // acceptance suite asserts the realized gains against an independent
    // transcription of the envelope math, which is only possible because these
    // are public.

    /// Number of taps currently live on `channel` (0 = L, 1 = R).
    int tap_count(int channel) const { return tap_count_[front_][channel & 1]; }

    /// Tap `index` on `channel`. Delay is in samples from the write head;
    /// `segment` is its spectral-tilt group.
    const Tap& tap(int channel, int index) const {
        return taps_[front_][channel & 1][static_cast<std::size_t>(index)];
    }

    /// The L1 normalization scalar solved for `channel`'s live tap table, so a
    /// caller can predict `|g_k|` without re-deriving the normalization.
    double tap_norm(int channel) const { return tap_norm_[front_][channel & 1]; }

    /// Window length in samples — the denominator of every `τ`.
    ///
    /// Every geometry accessor describes the bank currently being heard, not the
    /// most recent `set_*`. During the `kSwapFadeMs` window after a topology
    /// change the two differ, and reporting the new geometry against the old
    /// tap table would make `tap(ch, k).delay / window_samples()` a τ that
    /// belongs to neither.
    int window_samples() const { return bank_window_[front_]; }
    int predelay_samples() const { return bank_predelay_[front_]; }
    int allpass_length(int index) const { return allpass_len_[index]; }

    /// The worst-case gain for the current settings, as a closed form of the
    /// shipped constants (series law 8).
    double worst_case_gain() const {
        return nonlin_ambience::worst_case_gain(
            diffusion_, converter_.target() > SampleType{0});
    }

    NonlinProgram program() const { return program_; }
    double length_ms() const { return length_ms_; }
    double tone() const { return tone_; }
    bool swap_in_progress() const { return fading_; }

    /// The normalized envelope this instance's current settings ask for. The
    /// program-specific `h` / `r` are resolved from the percent parameters here
    /// so callers do not have to know which program reads which.
    double envelope(double tau) const {
        return nonlin_ambience::program_envelope(program_, tau, gate_hold_pct_ / 100.0,
                                                 attack_pct_ / 100.0);
    }

private:
    // ── Tap-cloud rendering ───────────────────────────────────────────────

    SampleType render_bank(int bank, int channel) {
        std::array<SampleType, nonlin_ambience::kSegments> acc{};
        const Tap* taps = taps_[bank][channel].data();
        const int count = tap_count_[bank][channel];
        const SampleType* ring = ring_.data();
        const int write = ring_write_;
        const int mask = ring_mask_;
        for (int k = 0; k < count; ++k)
            acc[static_cast<std::size_t>(taps[k].segment)] +=
                static_cast<SampleType>(taps[k].gain) *
                ring[static_cast<std::size_t>((write - taps[k].delay) & mask)];

        SampleType out{0};
        auto& state = seg_state_[bank][channel];
        for (int s = 0; s < nonlin_ambience::kSegments; ++s) {
            const SampleType a = seg_pole_[static_cast<std::size_t>(s)];
            state[static_cast<std::size_t>(s)] = snap_to_zero(
                a * state[static_cast<std::size_t>(s)] +
                (SampleType{1} - a) * acc[static_cast<std::size_t>(s)]);
            out += state[static_cast<std::size_t>(s)];
        }
        return out;
    }

    /// Bandlimit → quantize with TPDF dither → DC block. The one nonlinearity in
    /// the file; see the anti-aliasing note.
    SampleType converter_stage(int channel, SampleType x, SampleType amount) {
        namespace na = nonlin_ambience;
        SampleType y = conv_hp_[channel].process_highpass(x);
        y = conv_lp_[channel].process_lowpass(y);

        const int bits = na::kConverterBitsMax -
                         static_cast<int>(std::lround(
                             static_cast<double>(amount) * na::kConverterBitSweep));
        const auto step = static_cast<SampleType>(
            1.0 / static_cast<double>(1 << (std::max(2, bits) - 1)));
        // TPDF dither, 1 LSB peak: the difference of two independent uniforms.
        const SampleType d = tpdf_difference(dither_.next_unit<SampleType>(),
                                             dither_.next_unit<SampleType>()) * step;
        y = std::round((y + d) / step) * step;

        return conv_dc_[channel].process(y);
    }

    // ── Tap-table construction ────────────────────────────────────────────

    void rebuild_immediate() {
        topology_dirty_ = false;
        topology_build_.active = false;
        build_bank(front_);
        fading_ = false;
        fade_pos_ = 0;
    }

    void rebuild_and_swap() {
        if (ring_.empty()) return;  // not prepared yet
        topology_dirty_ = false;
        topology_build_.active = false;
        if (fading_) {
            // A second topology change inside one fade collapses the first: the
            // pending bank becomes live immediately so the back bank is free to
            // be rewritten. Documented, and only reachable by changing topology
            // twice within kSwapFadeMs.
            front_ = pending_;
            fading_ = false;
            fade_pos_ = 0;
        }
        const int back = 1 - front_;
        build_bank(back);
        ++topology_rebuild_count_;
        seg_state_[back][0].fill(SampleType{0});
        seg_state_[back][1].fill(SampleType{0});
        pending_ = back;
        fade_pos_ = 0;
        fading_ = true;
    }

    void build_bank(int bank) {
        namespace na = nonlin_ambience;

        const int window_samples = std::max(1, static_cast<int>(std::lround(
            units::ms_to_samples(length_ms_, sample_rate_))));
        const int predelay_samples = std::max(0, static_cast<int>(std::lround(
            units::ms_to_samples(predelay_ms_, sample_rate_))));
        bank_window_[bank] = window_samples;
        bank_predelay_[bank] = predelay_samples;

        const double gate_hold = gate_hold_pct_ / 100.0;
        const double attack = attack_pct_ / 100.0;
        for (int ch = 0; ch < 2; ++ch) {
            const std::uint32_t seed = ch == 0 ? seed_ : (seed_ ^ na::kSeedRodd);
            Tap* out = taps_[bank][ch].data();
            int count = 0;
            double l1 = 0.0;

            double t = 0.0;  // position inside the window, in samples
            int index = 0;
            while (t < static_cast<double>(window_samples) && count < tap_capacity_) {
                const double u = t / static_cast<double>(window_samples);
                const double nd = na::pulse_density(u, density_pct_, density_growth_);
                const double grid = sample_rate_ / nd;  // Td, in samples

                const auto design = na::design_velvet_tap(
                    program_, t, grid, window_samples, predelay_samples,
                    gate_hold, attack,
                    velvet_noise_draw<double>(seed, static_cast<std::uint64_t>(index)));

                // A tap whose envelope is exactly zero is past a hard gate. It
                // is dropped rather than stored, which is what makes the "IR is
                // zero after the last tap" property meaningful: the last stored
                // tap is the last audible one.
                if (design.audible) {
                    // sqrt(Td) density weighting — see the file note. It makes
                    // the short-time RMS envelope equal E(τ) regardless of how
                    // the density law varies underneath it.
                    const double magnitude = design.magnitude;
                    out[count].delay = design.delay;
                    out[count].gain = static_cast<float>(design.gain);
                    out[count].segment = design.segment;
                    l1 += magnitude;
                    ++count;
                }

                t += grid;
                ++index;
            }

            // §4.4: solve `norm` so Σ|g_k| == G_L1 exactly. That single
            // constraint sets both the loudness and the peak-gain bound.
            const double norm = l1 > na::kNormFloor ? na::kL1Budget / l1 : 0.0;
            for (int k = 0; k < count; ++k)
                out[k].gain = static_cast<float>(static_cast<double>(out[k].gain) * norm);

            tap_count_[bank][ch] = count;
            tap_norm_[bank][ch] = norm;
        }
    }

    struct IncrementalTopologyBuild {
        bool active = false;
        int bank = 0;
        int channel = 0;
        bool normalizing = false;
        int window_samples = 1;
        int predelay_samples = 0;
        int index = 0;
        int count = 0;
        int normalize_index = 0;
        double t = 0.0;
        double l1 = 0.0;
        double norm = 0.0;
        NonlinProgram program = NonlinProgram::ambience;
        double density_pct = nonlin_ambience::kDensityRefPct;
        double density_growth = nonlin_ambience::kGammaDefault;
        double gate_hold = nonlin_ambience::kGateHold;
        double attack = nonlin_ambience::kRevRise;
    };

    void begin_incremental_topology_rebuild() {
        auto& b = topology_build_;
        b = {};
        b.active = true;
        b.bank = 1 - front_;
        b.window_samples = std::max(1, static_cast<int>(std::lround(
            units::ms_to_samples(length_ms_, sample_rate_))));
        b.predelay_samples = std::max(0, static_cast<int>(std::lround(
            units::ms_to_samples(predelay_ms_, sample_rate_))));
        b.program = program_;
        b.density_pct = density_pct_;
        b.density_growth = density_growth_;
        b.gate_hold = gate_hold_pct_ / 100.0;
        b.attack = attack_pct_ / 100.0;
        bank_window_[b.bank] = b.window_samples;
        bank_predelay_[b.bank] = b.predelay_samples;
    }

    void finish_incremental_channel() {
        auto& b = topology_build_;
        tap_count_[b.bank][b.channel] = b.count;
        tap_norm_[b.bank][b.channel] = b.norm;
        if (++b.channel < 2) {
            b.normalizing = false;
            b.index = 0;
            b.count = 0;
            b.normalize_index = 0;
            b.t = 0.0;
            b.l1 = 0.0;
            b.norm = 0.0;
            return;
        }
        seg_state_[b.bank][0].fill(SampleType{0});
        seg_state_[b.bank][1].fill(SampleType{0});
        pending_ = b.bank;
        fade_pos_ = 0;
        fading_ = true;
        b.active = false;
        topology_dirty_ = false;
        ++topology_rebuild_count_;
    }

    void advance_incremental_topology_unit() {
        namespace na = nonlin_ambience;
        auto& b = topology_build_;
        Tap* out = taps_[b.bank][b.channel].data();
        if (b.normalizing) {
            if (b.normalize_index < b.count) {
                auto& gain = out[b.normalize_index++].gain;
                gain = static_cast<float>(static_cast<double>(gain) * b.norm);
            } else {
                finish_incremental_channel();
            }
            return;
        }
        if (b.t >= static_cast<double>(b.window_samples) || b.count >= tap_capacity_) {
            b.norm = b.l1 > na::kNormFloor ? na::kL1Budget / b.l1 : 0.0;
            b.normalizing = true;
            b.normalize_index = 0;
            return;
        }
        const double u = b.t / static_cast<double>(b.window_samples);
        const double nd = na::pulse_density(u, b.density_pct, b.density_growth);
        const double grid = sample_rate_ / nd;
        const std::uint32_t channel_seed =
            b.channel == 0 ? seed_ : (seed_ ^ na::kSeedRodd);
        const auto design = na::design_velvet_tap(
            b.program, b.t, grid, b.window_samples, b.predelay_samples,
            b.gate_hold, b.attack,
            velvet_noise_draw<double>(channel_seed, static_cast<std::uint64_t>(b.index)));
        if (design.audible) {
            const double magnitude = design.magnitude;
            out[b.count].delay = design.delay;
            out[b.count].gain = static_cast<float>(design.gain);
            out[b.count].segment = design.segment;
            b.l1 += magnitude;
            ++b.count;
        }
        b.t += grid;
        ++b.index;
    }

    void advance_topology_rebuild() {
        topology_work_units_last_sample_ = 0;
        if (fading_) return;
        if (!topology_build_.active) {
            if (!topology_dirty_) return;
            begin_incremental_topology_rebuild();
        }
        for (int i = 0; i < kTopologyWorkPerSample && topology_build_.active; ++i) {
            advance_incremental_topology_unit();
            ++topology_work_units_last_sample_;
        }
    }

    // ── Segment tilt ──────────────────────────────────────────────────────

    void update_segment_coefficients() {
        namespace na = nonlin_ambience;
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        const double bright = na::kFcBright;
        const double dark = hf_damp_hz_;
        for (int s = 0; s < na::kSegments; ++s) {
            const double q = std::clamp(
                static_cast<double>(s) / static_cast<double>(na::kSegments - 1) -
                    0.5 * tone_,
                0.0, 1.0);
            const double fc = bright * std::pow(dark / bright, q);
            // §4.5's stated coefficient: the impulse-invariant pole.
            seg_pole_[static_cast<std::size_t>(s)] = static_cast<SampleType>(
                std::exp(-kTwoPi * fc / sample_rate_));
        }
    }

    // ── Small helpers ─────────────────────────────────────────────────────

    static std::size_t next_pow2(std::size_t n) {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    static bool is_prime(int n) {
        if (n < 2) return false;
        if (n % 2 == 0) return n == 2;
        for (int d = 3; d * d <= n; d += 2)
            if (n % d == 0) return false;
        return true;
    }

    /// Nudges a delay to the nearest prime sample count so the two allpass combs
    /// share no period and their echoes never align.
    static int nearest_prime(int n) {
        for (int offset = 0; offset < 64; ++offset) {
            if (is_prime(n + offset)) return n + offset;
            if (n - offset > 1 && is_prime(n - offset)) return n - offset;
        }
        return n;
    }

    // ── State ─────────────────────────────────────────────────────────────

    double sample_rate_ = 48000.0;
    double max_length_ms_ = nonlin_ambience::kMaxLengthMs;
    double max_predelay_ms_ = nonlin_ambience::kMaxPredelayMs;

    NonlinProgram program_ = NonlinProgram::ambience;
    double length_ms_ = 350.0;
    double predelay_ms_ = 0.0;
    double density_pct_ = nonlin_ambience::kDensityRefPct;
    double density_growth_ = nonlin_ambience::kGammaDefault;
    double diffusion_ = nonlin_ambience::kDiffusionDefault;
    double gate_hold_pct_ = nonlin_ambience::kGateHold * 100.0;
    double attack_pct_ = nonlin_ambience::kRevRise * 100.0;
    std::uint32_t seed_ = nonlin_ambience::kDefaultSeed;
    double tone_ = 0.0;
    double hf_damp_hz_ = nonlin_ambience::kFcDark;

    int tap_capacity_ = 0;

    std::vector<Tap> taps_[2][2];
    int tap_count_[2][2] = {{0, 0}, {0, 0}};
    double tap_norm_[2][2] = {{0.0, 0.0}, {0.0, 0.0}};
    // Geometry is stored PER BANK so the introspection accessors can describe
    // the bank being heard rather than the most recent `set_*`.
    int bank_window_[2] = {0, 0};
    int bank_predelay_[2] = {0, 0};

    std::vector<SampleType> ring_;
    int ring_mask_ = 0;
    int ring_write_ = 0;

    DelayLineT<SampleType> allpass_[nonlin_ambience::kNumAllpass];
    int allpass_len_[nonlin_ambience::kNumAllpass] = {0, 0};

    std::array<SampleType, nonlin_ambience::kSegments> seg_pole_{};
    std::array<std::array<SampleType, nonlin_ambience::kSegments>, 2> seg_state_[2]{};

    int front_ = 0;
    int pending_ = 0;
    bool fading_ = false;
    int fade_pos_ = 0;
    std::uint64_t topology_rebuild_count_ = 0;
    bool topology_dirty_ = false;
    IncrementalTopologyBuild topology_build_{};
    int topology_work_units_last_sample_ = 0;
    int swap_fade_len_ = 1;

    SmoothedValue<SampleType> mix_{SampleType{1}};
    SmoothedValue<SampleType> width_{SampleType{1}};
    SmoothedValue<SampleType> output_gain_{SampleType{1}};
    SmoothedValue<SampleType> converter_{SampleType{0}};

    TptFilterT<SampleType> conv_hp_[2];
    TptFilterT<SampleType> conv_lp_[2];
    DcBlocker<SampleType> conv_dc_[2];
    Xorshift32 dither_{0x2C1B3A5Du};
};

using NonlinAmbience = NonlinAmbienceT<float>;
using NonlinAmbience64 = NonlinAmbienceT<double>;

}  // namespace pulp::signal
