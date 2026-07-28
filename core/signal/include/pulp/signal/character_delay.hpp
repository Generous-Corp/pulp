#pragma once

// Multi-character delay — one engine, five characters.
//
// The premise: what makes a delay sound like a Space Echo, a Memory Man, or an
// early-80s rack unit is not the delay, it is what happens to a repeat as it
// CIRCULATES. So the frame here (dual delay lines, feedback, crossfeed,
// modulation, reverse/freeze/ducking) is shared, and each character plugs its
// own processing INSIDE the feedback loop.
//
// That placement is the whole trick. In-loop coloration accumulates
// exponentially per repeat — repeat 1 has been through the character once,
// repeat 10 has been through it ten times — and that accumulation is the
// vintage sound. The same processing placed after the delay would colour every
// repeat identically and sound like an effect on a delay, not like a machine.
//
//   * Clean          — transparent full-bandwidth repeats. The null-test
//                      baseline and the reference modern digital delay.
//   * Vintage Digital— a modeled converter loop at a reduced internal rate:
//                      band-limited, pre-emphasized, low-bit. Time changes
//                      glide in pitch because the converter is clocked.
//   * Tape           — wow/flutter instability, EQ-bracketed saturation that
//                      compounds per pass, head-bump low-mids, dark decaying
//                      repeats, long motor-lag swoops. Self-oscillates
//                      musically. A physical tier adds true magnetic
//                      hysteresis, physics-derived loss and wear artifacts.
//   * BBD            — bandwidth tied to clock rate (short = bright, long =
//                      dark), compander pumping, clock artifacts.
//   * Diffusion      — repeats smear into soft clouds, approaching reverb
//                      texture as they recirculate.
//
// Concept-to-citation map for the mechanisms used here:
//   fractional-delay interpolation ....... Laakso et al. 1996; Dattorro 1997 Pt 2
//   allpass diffusion + network values ... Schroeder 1962; Dattorro 1997 Pt 1
//   BBD device / compander / bandwidth ... Raffel & Smith DAFx-10;
//                                          Holters & Parker DAFx-18; NE570 datasheet
//   BBD stage counts ..................... Panasonic MN3005/3007/3008 datasheets
//   tape model family, oversampling ...... Chowdhury DAFx-19 (2019)
//   hysteresis physics + constants ....... Jiles & Atherton 1986;
//                                          Jiles, Thoelke & Devine 1992
//   playback loss physics ................ Wallace 1951; Bertram 1994
//   drift process ........................ Uhlenbeck & Ornstein 1930
//   antiquing artifacts .................. Välimäki et al. JAES 2008
//   dither correctness ................... Lipshitz/Wannamaker/Vanderkooy 1992
//   emphasis time constant ............... IEC 60908
//   PRNG / normal deviates ............... Marsaglia 2003; Box & Muller 1958
//   SVF / TPT filter forms ............... Zavalishin; Simper (Cytomic)
//
// Contract:
//   * WET ONLY. Dry/wet mixing is the graph's job (make_drywet_node()), so
//     there is deliberately no mix parameter in the DSP.
//   * Zero allocations in process() and reset(). Every buffer is sized in
//     set_sample_rate() for the 2 s maximum, including the reverse
//     double-buffers and both tape tiers. That costs a few megabytes per
//     stereo instance and buys an audio thread that never touches an
//     allocator, which is the right trade for a module this size.
//   * latency_samples() is 0 in every configuration. The physical tier's
//     hysteresis oversampler has a fixed group delay, but it lives INSIDE the
//     loop and is folded out of the delay line's read distance rather than
//     reported to the host.
//   * Delay-time changes always glide. Every character reads through a slewed
//     continuous time, so the pitch shift is physically correct and per
//     character; there is no crossfade-jump path anywhere.

#include <pulp/signal/character_delay/bbd.hpp>
#include <pulp/signal/character_delay/diffusion.hpp>
#include <pulp/signal/character_delay/primitives.hpp>
#include <pulp/signal/character_delay/reverse.hpp>
#include <pulp/signal/character_delay/tables.hpp>
#include <pulp/signal/character_delay/tape.hpp>
#include <pulp/signal/character_delay/tape_physical.hpp>
#include <pulp/signal/character_delay/vintage.hpp>
#include <pulp/signal/svf.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::signal {

template <typename SampleType>
class CharacterDelayT {
public:
    enum class Character { clean, vintage_digital, tape, bbd, diffusion };
    enum class TapeTier { standard, physical };

    // Thread contract: this object is deliberately lock-free and is not safe
    // for concurrent mutation. Call set_sample_rate()/set_character()/the tape
    // configuration methods while processing is stopped. Character selection
    // is switchable between realizations/processing runs, not concurrently in
    // a live render. Parameter setters are
    // realtime-safe and belong on the SAME audio thread that calls process()
    // (the catalog adapter follows this contract). A host whose controls arrive
    // on another thread must publish them through its parameter store before
    // the audio thread calls these setters.

    // ── Configuration (processing stopped) ────────────────────────────────

    /// Allocates every buffer for the 2 s left-channel maximum times the
    /// largest right-channel offset, recomputes coefficients, then resets.
    /// Must be called before process().
    void set_sample_rate(double sample_rate) {
        if (!std::isfinite(sample_rate)) return;
        sample_rate_ = std::max(sample_rate, 1000.0);
        const auto line_capacity =
            static_cast<std::size_t>(
                std::ceil(chardelay::kMaxAddressableDelayMs * 0.001 * sample_rate_) + 4.0);

        for (auto& channel : channels_) {
            channel.line.prepare(line_capacity);
            channel.reverse.prepare(line_capacity);
            channel.diffusion.prepare(sample_rate_);
            channel.bbd.prepare(sample_rate_);
            channel.vintage.prepare(sample_rate_);
            channel.tape.prepare(sample_rate_);
            channel.tape_physical.prepare(sample_rate_);
            channel.instability.prepare(sample_rate_);
        }
        channels_[0].instability.configure(chardelay::kPrngSeed, 0.0);
        channels_[1].instability.configure(chardelay::kPrngSeed ^ 0x5BF03635u, chardelay::kFlutterPhaseRight);
        channels_[0].bbd.set_seed(chardelay::kPrngSeed);
        channels_[1].bbd.set_seed(chardelay::kPrngSeed ^ 0x27D4EB2Fu);
        channels_[0].vintage.set_seed(chardelay::kPrngSeed);
        channels_[1].vintage.set_seed(chardelay::kPrngSeed ^ 0x165667B1u);
        channels_[0].tape_physical.set_seeds(chardelay::kPrngSeed);
        channels_[1].tape_physical.set_seeds(chardelay::kPrngSeed ^ 0x2545F491u);

        for (auto* smoother : {&feedback_, &crossfeed_, &duck_, &freeze_, &character_,
                               &diffusion_amount_})
            smoother->configure(chardelay::kParamGlideS, sample_rate_);

        duck_follower_.configure(chardelay::kDuckAttackS, chardelay::kDuckReleaseS,
                                 sample_rate_ / static_cast<double>(chardelay::kControlInterval));
        configure_time_slew();
        apply_tape_speed();
        update_loop_tone();
        reset();
    }

    void set_character(Character character) {
        character_type_ = character;
        configure_time_slew();
    }

    void set_tape_tier(TapeTier tier) { tape_tier_ = tier; }

    void set_tape_speed_ips(SampleType ips) {
        tape_speed_ips_ = std::clamp(finite_or(static_cast<double>(ips), tape_speed_ips_),
                                     chardelay::kTapeSpeedsIps.front(),
                                     chardelay::kTapeSpeedsIps.back());
        apply_tape_speed();
    }

    // ── Parameters (audio thread; smoothed in process()) ──────────────────

    void set_time_ms(SampleType left_ms) {
        time_ms_ = std::clamp(finite_or(static_cast<double>(left_ms), time_ms_), 1.0,
                              chardelay::kMaxDelayMs);
    }

    /// Right-channel time as a MULTIPLE of the left. Selects ratio mode; the
    /// right channel tracks the left, which is what a stereo-spread delay
    /// wants.
    void set_time_offset(SampleType multiplier) {
        time_offset_ = std::clamp(finite_or(static_cast<double>(multiplier), time_offset_),
                                  chardelay::kTimeOffsetMin,
                                  chardelay::kTimeOffsetMax);
        right_time_absolute_ = false;
    }

    /// Right-channel time in MILLISECONDS, independent of the left. Selects
    /// absolute mode, which is what a dual-mono setup (two unrelated delay
    /// times) and a millisecond offset both need — neither can be expressed as
    /// a ratio, because a fixed ratio is a different millisecond spread at
    /// every delay time and a fixed spread is a different ratio.
    ///
    /// Ratio mode is restored by calling set_time_offset().
    void set_right_time_ms(SampleType right_ms) {
        right_time_ms_ = std::clamp(finite_or(static_cast<double>(right_ms), right_time_ms_), 1.0,
                                    chardelay::kMaxAddressableDelayMs);
        right_time_absolute_ = true;
    }

    /// True while the right channel is following set_right_time_ms().
    bool right_time_is_absolute() const noexcept { return right_time_absolute_; }

    /// The right channel's target time in ms, whichever mode is selected.
    double right_time_ms() const noexcept {
        return right_time_absolute_ ? right_time_ms_ : time_ms_ * time_offset_;
    }

    void set_feedback(SampleType feedback) {
        feedback_target_ = std::clamp(
            finite_or(static_cast<double>(feedback), feedback_target_), 0.0,
            chardelay::kSaturatedFeedbackMax);
    }

    void set_crossfeed(SampleType crossfeed) {
        crossfeed_target_ =
            std::clamp(finite_or(static_cast<double>(crossfeed), crossfeed_target_), 0.0, 1.0);
    }

    void set_character_amount(SampleType amount) {
        character_target_ =
            std::clamp(finite_or(static_cast<double>(amount), character_target_), 0.0, 1.0);
    }

    // Cross-character diffusion: smears ANY character's repeats through the
    // diffusion network by this 0..1 amount, in-loop (same placement the
    // diffusion character uses). 0 leaves the selected character untouched.
    void set_diffusion_amount(SampleType amount) {
        diffusion_amount_target_ =
            std::clamp(finite_or(static_cast<double>(amount), diffusion_amount_target_), 0.0, 1.0);
    }

    void set_mod(SampleType rate01, SampleType depth01) {
        if (const double candidate = static_cast<double>(rate01); std::isfinite(candidate)) {
            const double rate = std::clamp(candidate, 0.0, 1.0);
            mod_rate_hz_ = chardelay::kModRateMinHz *
                           std::pow(chardelay::kModRateMaxHz /
                                        chardelay::kModRateMinHz,
                                    rate);
        }
        if (const double candidate = static_cast<double>(depth01); std::isfinite(candidate))
            mod_depth_ = std::clamp(candidate, 0.0, 1.0) * chardelay::kModMaxDepth;
    }

    void set_duck(SampleType amount01) {
        duck_target_ =
            std::clamp(finite_or(static_cast<double>(amount01), duck_target_), 0.0, 1.0);
    }

    void set_freeze(bool on) { freeze_on_ = on; }
    void set_reverse(bool on) { reverse_on_ = on; }

    // ── In-loop tone (audio thread) ───────────────────────────────────────
    //
    // A resonant 12 dB/oct high-pass (low cut) and low-pass (high cut) in the
    // RECIRCULATION path, so each repeat is filtered again and the tail evolves
    // — the behavior a tone control after the delay cannot produce. Orthogonal
    // to the character's own loop bandwidth: this is the player's control, that
    // is the realization's model.
    //
    // Bypassed bit-exactly while low cut is at its minimum and high cut at its
    // maximum, so an untouched delay is unchanged. Also bypassed under freeze,
    // which needs a unity loop to hold its content; the filters keep running so
    // their state stays coherent when freeze releases.
    void set_loop_low_cut_hz(SampleType hz) {
        const double next = std::clamp(finite_or(static_cast<double>(hz), loop_low_cut_hz_),
                                      chardelay::kLoopToneHpMinHz, chardelay::kLoopToneHpMaxHz);
        if (next == loop_low_cut_hz_) return;   // per-block pushes are free
        loop_low_cut_hz_ = next;
        update_loop_tone();
    }

    void set_loop_low_cut_resonance(SampleType q) {
        const double next = std::clamp(finite_or(static_cast<double>(q), loop_low_cut_q_),
                                     chardelay::kLoopToneResMin, chardelay::kLoopToneResMax);
        if (next == loop_low_cut_q_) return;   // per-block pushes are free
        loop_low_cut_q_ = next;
        update_loop_tone();
    }

    void set_loop_high_cut_hz(SampleType hz) {
        const double next = std::clamp(finite_or(static_cast<double>(hz), loop_high_cut_hz_),
                                       chardelay::kLoopToneLpMinHz, chardelay::kLoopToneLpMaxHz);
        if (next == loop_high_cut_hz_) return;   // per-block pushes are free
        loop_high_cut_hz_ = next;
        update_loop_tone();
    }

    void set_loop_high_cut_resonance(SampleType q) {
        const double next = std::clamp(finite_or(static_cast<double>(q), loop_high_cut_q_),
                                      chardelay::kLoopToneResMin, chardelay::kLoopToneResMax);
        if (next == loop_high_cut_q_) return;   // per-block pushes are free
        loop_high_cut_q_ = next;
        update_loop_tone();
    }

    /// True when the in-loop tone stage is doing anything at all.
    bool loop_tone_active() const noexcept { return loop_tone_active_; }

    // ── Runtime ───────────────────────────────────────────────────────────

    /// Always 0. See the header note: the physical tier's in-loop oversampler
    /// group delay is folded into the delay line, and every other stage is IIR
    /// (group delay is phase, not a bufferable constant).
    int latency_samples() const noexcept { return 0; }

    /// Allocation-free. Zeroes buffers and filter/solver state, re-seeds every
    /// PRNG, and snaps smoothed values to their targets, so two renders after a
    /// reset() are bit-identical.
    /// Clear realtime state on the audio thread between blocks, or while
    /// processing is stopped. Never race this against process().
    void reset() noexcept {
        for (auto& channel : channels_) {
            channel.line.reset();
            channel.reverse.reset();
            channel.diffusion.reset();
            channel.bbd.reset();
            channel.vintage.reset();
            channel.tape.reset();
            channel.tape_physical.reset();
            channel.instability.reset();
            channel.loop_highpass.reset();
            channel.loop_lowpass.reset();
            channel.tone_high_pass.reset();
            channel.tone_low_pass.reset();
            channel.wet = 0.0;
        }
        feedback_.snap(feedback_target_);
        crossfeed_.snap(crossfeed_target_);
        duck_.snap(duck_target_);
        character_.snap(character_target_);
        diffusion_amount_.snap(diffusion_amount_target_);
        freeze_.snap(freeze_on_ ? 1.0 : 0.0);

        duck_follower_.reset(0.0);
        duck_gain_ = 1.0;
        duck_gain_step_ = 0.0;
        duck_peak_ = 0.0;
        lfo_phase_ = 0.0;
        bbd_flutter_phase_ = 0.0;
        control_counter_ = 0;

        time_slew_[0].snap(time_ms_);
        time_slew_[1].snap(right_time_ms());
        update_control_rate();
    }

    /// Stereo, in place, WET ONLY.
    void process(SampleType* left, SampleType* right, int num_samples) noexcept {
        if (left == nullptr || right == nullptr) return;
        for (int i = 0; i < num_samples; ++i) {
            if (control_counter_-- <= 0) {
                control_counter_ = chardelay::kControlInterval - 1;
                update_control_rate();
            }

            const double character_amount = character_.process(character_target_);
            diffusion_amount_value_ = diffusion_amount_.process(diffusion_amount_target_);
            const double freeze = freeze_.process(freeze_on_ ? 1.0 : 0.0);
            duck_.process(duck_target_);

            // Crossfeed is forced to zero in reverse: the channels hold
            // independently segmented buffers there, so "bouncing" between them
            // exchanges unrelated fragments and collapses toward mono instead
            // of producing a stereo bounce. It glides to zero through the same
            // smoother rather than stepping, so engaging reverse on a
            // ping-ponging delay is not itself a click.
            const double crossfeed = crossfeed_.process(reverse_on_ ? 0.0 : crossfeed_target_);

            double feedback = std::min(feedback_.process(feedback_target_), feedback_ceiling());
            if (physical_tape())
                feedback = chardelay::physical_tape_effective_feedback(
                    feedback, character_amount);
            // Freeze overrides the parameter AND the per-character clamp: the
            // loop must be exactly unity for the frozen content to hold.
            feedback += freeze * (1.0 - feedback);

            const double in_left = static_cast<double>(left[i]);
            const double in_right = static_cast<double>(right[i]);
            duck_peak_ = std::max(duck_peak_, std::max(std::abs(in_left), std::abs(in_right)));

            const double tap_left =
                (1.0 - crossfeed) * channels_[0].wet + crossfeed * channels_[1].wet;
            const double tap_right =
                (1.0 - crossfeed) * channels_[1].wet + crossfeed * channels_[0].wet;
            const double input_gain = 1.0 - freeze;
            const double line_in_left = in_left * input_gain + loop_saturate(tap_left * feedback);
            const double line_in_right =
                in_right * input_gain + loop_saturate(tap_right * feedback);

            advance_modulation(character_amount);
            const double time_left = modulated_time_ms(0, character_amount);
            const double time_right = modulated_time_ms(1, character_amount);

            channels_[0].wet =
                process_character(channels_[0], line_in_left, time_left, character_amount, 1.0);
            channels_[1].wet = process_character(channels_[1], line_in_right, time_right,
                                                 character_amount, chardelay::kStereoDecorr);

            // The player's tone stage sits on the delay's OUTPUT, which is also
            // what the feedback tap reads. So the very first repeat is filtered
            // once — a high cut at its minimum really does mute the character —
            // and every recirculation is filtered again on top, which is what
            // makes the tail keep darkening. Filtering only the feedback path
            // would leave that first repeat unfiltered and let the character
            // through no matter where the cut sits.
            channels_[0].wet = apply_loop_tone(channels_[0], channels_[0].wet, freeze);
            channels_[1].wet = apply_loop_tone(channels_[1], channels_[1].wet, freeze);

            // Ducking is applied to the WET output only, sidechained from the
            // dry input before the loop — so repeats bloom in the gaps rather
            // than being pushed down along with the source.
            duck_gain_ = std::clamp(duck_gain_ + duck_gain_step_, 0.0, 1.0);
            const double gain = duck_gain_;

            left[i] = static_cast<SampleType>(channels_[0].wet * gain);
            right[i] = static_cast<SampleType>(channels_[1].wet * gain);
        }
    }

    // ── Introspection and test hooks ──────────────────────────────────────
    /// Bypass the BBD compander. Proving the compander does anything requires
    /// comparing against the same signal path without it.
    void set_bbd_compander_enabled(bool enabled) noexcept {
        for (auto& channel : channels_) channel.bbd.set_compander_enabled(enabled);
    }

    double bbd_bandwidth_hz() const noexcept { return channels_[0].bbd.bandwidth_hz(); }
    std::size_t bbd_stages() const noexcept { return channels_[0].bbd.stages(); }
    double vintage_band_edge_hz() const noexcept { return channels_[0].vintage.band_edge_hz(); }
    double vintage_internal_rate_hz() const noexcept {
        return channels_[0].vintage.internal_rate_hz();
    }
    std::size_t chew_state_index(int channel) const noexcept {
        return channels_[static_cast<std::size_t>(channel)].tape_physical.chew_state_index();
    }
    const std::vector<double>& tape_gap_coefficients(int channel) const noexcept {
        return channels_[static_cast<std::size_t>(channel)].tape_physical.gap_coefficients();
    }
private:
    static double finite_or(double value, double fallback) noexcept {
        return std::isfinite(value) ? value : fallback;
    }

    struct ChannelState {
        chardelay::FractionalDelayLine line;
        chardelay::ReverseSegmenter reverse;
        chardelay::DiffusionChain diffusion;
        chardelay::BbdChannel bbd;
        chardelay::VintageChannel vintage;
        chardelay::TapeStandardChannel tape;
        chardelay::TapePhysicalChannel tape_physical;
        chardelay::TapeInstability instability;
        chardelay::OnePole loop_highpass;
        chardelay::OnePole loop_lowpass;
        // Player-facing in-loop tone (see set_loop_low_cut_hz). Separate from
        // loop_highpass/loop_lowpass above, which the character owns.
        Svf64 tone_high_pass;
        Svf64 tone_low_pass;
        double wet = 0.0;
    };

    /// Retune both loop-tone filters and latch whether the stage does anything.
    /// Fully-open controls bypass it so an untouched delay stays bit-exact.
    void update_loop_tone() noexcept {
        loop_tone_active_ = loop_low_cut_hz_ > chardelay::kLoopToneHpMinHz ||
                            loop_high_cut_hz_ < chardelay::kLoopToneLpMaxHz;
        for (auto& channel : channels_) {
            channel.tone_high_pass.set_mode(Svf64::Mode::highpass);
            channel.tone_high_pass.set_sample_rate(sample_rate_);
            channel.tone_high_pass.set_frequency(loop_low_cut_hz_);
            channel.tone_high_pass.set_resonance(loop_low_cut_q_);
            channel.tone_low_pass.set_mode(Svf64::Mode::lowpass);
            channel.tone_low_pass.set_sample_rate(sample_rate_);
            channel.tone_low_pass.set_frequency(loop_high_cut_hz_);
            channel.tone_low_pass.set_resonance(loop_high_cut_q_);
        }
    }

    /// One pass of the player's tone stage over the recirculating signal.
    /// @p freeze crossfades back to the unfiltered signal so a frozen loop
    /// keeps the exactly-unity gain it needs to hold, while the filter states
    /// stay warm for the moment freeze releases.
    double apply_loop_tone(ChannelState& channel, double x, double freeze) noexcept {
        if (!loop_tone_active_) return x;
        double y = x;
        if (loop_low_cut_hz_ > chardelay::kLoopToneHpMinHz)
            y = channel.tone_high_pass.process(y);
        if (loop_high_cut_hz_ < chardelay::kLoopToneLpMaxHz)
            y = channel.tone_low_pass.process(y);
        return freeze >= 1.0 ? x : y + freeze * (x - y);
    }

    bool physical_tape() const noexcept {
        return character_type_ == Character::tape && tape_tier_ == TapeTier::physical;
    }

    /// Characters with an in-loop saturator keep the full 0–1.1 range: the
    /// nonlinearity bounds the energy, and >unity feedback into saturation is
    /// the classic self-oscillating dub behavior. Characters without one are
    /// clamped below unity, because for a linear loop stability IS the clamp.
    double feedback_ceiling() const noexcept {
        switch (character_type_) {
            case Character::tape:
            case Character::bbd:
            case Character::vintage_digital:
                return chardelay::kSaturatedFeedbackMax;
            case Character::clean:
            case Character::diffusion:
            default:
                return chardelay::kUnsaturatedFeedbackMax;
        }
    }

    double loop_saturate(double v) const noexcept {
        switch (character_type_) {
            case Character::tape:
            case Character::bbd:
            case Character::vintage_digital:
                return chardelay::fast_tanh(v);
            case Character::clean:
            case Character::diffusion:
            default:
                return v;
        }
    }

    double time_slew_seconds() const noexcept {
        switch (character_type_) {
            case Character::vintage_digital: return chardelay::kTimeSlewVintageMs * 0.001;
            case Character::tape:            return chardelay::kTimeSlewTapeMs * 0.001;
            case Character::bbd:             return chardelay::kTimeSlewBbdMs * 0.001;
            case Character::diffusion:       return chardelay::kTimeSlewDiffusionMs * 0.001;
            case Character::clean:
            default:                         return chardelay::kTimeSlewCleanMs * 0.001;
        }
    }

    void configure_time_slew() {
        for (auto& slew : time_slew_) slew.configure(time_slew_seconds(), sample_rate_);
    }

    void apply_tape_speed() {
        for (auto& channel : channels_) channel.tape_physical.set_speed_ips(tape_speed_ips_);
    }

    /// Coefficient recomputation for the ACTIVE character only. Running every
    /// character's update would multiply the cost of a knob turn by five for no
    /// audible benefit — a character that is not in the signal path cannot be
    /// heard, and switching to it re-runs this on the next control tick anyway.
    void update_control_rate() noexcept {
        const double amount = character_.current();

        // Ducking: a control-rate follower fed the interval's PEAK, so a
        // transient between ticks still opens the ducker. The resulting gain is
        // ramped across the next interval rather than stepped.
        // The gain law consumes only env/threshold clamped to [0, 1]. Letting
        // the follower retain values above that ceiling cannot increase the
        // duck, but it does make release time depend on how far above the
        // threshold the source happened to peak. Cap at the last meaningful
        // detector value so every fully-ducked passage recovers on the stated
        // release constant (and therefore within its three-tau contract).
        const double detector = std::min(duck_peak_, chardelay::kDuckThreshold);
        const double envelope = duck_follower_.process(detector);
        duck_peak_ = 0.0;
        const double normalized = std::clamp(envelope / chardelay::kDuckThreshold, 0.0, 1.0);
        const double knee = normalized * normalized * (3.0 - 2.0 * normalized);
        const double target = 1.0 - duck_.current() * knee;
        duck_gain_step_ = (target - duck_gain_) / static_cast<double>(chardelay::kControlInterval);

        switch (character_type_) {
            case Character::clean: {
                const double lowpass_hz =
                    chardelay::interpolate_knots(chardelay::kCleanAxis, chardelay::kCleanLoopLpHz, amount);
                const double highpass_hz =
                    chardelay::interpolate_knots(chardelay::kCleanAxis, chardelay::kCleanLoopHpHz, amount);
                clean_lowpass_bypassed_ = lowpass_hz >= chardelay::kLoopLpBypassHz;
                for (auto& channel : channels_) {
                    channel.loop_highpass.set_cutoff(highpass_hz, sample_rate_);
                    channel.loop_lowpass.set_cutoff(lowpass_hz, sample_rate_);
                }
                break;
            }
            case Character::diffusion:
                clean_lowpass_bypassed_ = false;
                for (auto& channel : channels_) {
                    channel.loop_highpass.set_cutoff(chardelay::kDiffusionLoopHpHz, sample_rate_);
                    channel.loop_lowpass.set_cutoff(chardelay::kDiffusionLoopLpHz, sample_rate_);
                }
                break;
            case Character::tape:
                for (auto& channel : channels_) {
                    if (physical_tape()) {
                        channel.tape_physical.update(amount);
                    } else {
                        channel.tape.update(amount);
                    }
                }
                break;
            case Character::bbd:
                // In reverse the segmenter carries the requested time and the
                // clocked line runs short and fixed, so the two delays sum to
                // roughly the requested time rather than double it.
                for (std::size_t c = 0; c < channels_.size(); ++c)
                    channels_[c].bbd.update(amount, reverse_on_
                                                        ? chardelay::kReverseLineMs * 0.001
                                                        : time_slew_[c].current() * 0.001);
                break;
            case Character::vintage_digital:
                for (auto& channel : channels_) channel.vintage.update(amount);
                break;
        }

        // The diffusion network is now engaged for EVERY character (scaled by the
        // cross-character diffusion amount in process_character), so its tank mix
        // is updated here unconditionally rather than only in the diffusion case.
        // Cheap, control-rate, idempotent.
        for (auto& channel : channels_)
            channel.diffusion.update(diffusion_amount_.current());
    }

    void advance_modulation(double character_amount) noexcept {
        lfo_phase_ += mod_rate_hz_ / sample_rate_;
        if (lfo_phase_ >= 1.0) lfo_phase_ -= 1.0;

        if (character_type_ == Character::bbd) {
            const double rate = chardelay::kBbdFlutterRateBaseHz + chardelay::kBbdFlutterRateSpanHz * character_amount;
            bbd_flutter_phase_ += rate / sample_rate_;
            if (bbd_flutter_phase_ >= 1.0) bbd_flutter_phase_ -= 1.0;
        }
    }

    /// Target time → slew → modulation. The BBD's own flutter is applied to the
    /// TARGET (before the slew) because it is clock-rate wander, not read-head
    /// motion: it has to pass through the same transport inertia the time knob
    /// does.
    double modulated_time_ms(std::size_t channel, double character_amount) noexcept {
        double target = (channel == 0) ? time_ms_ : right_time_ms();
        const double decorrelation = (channel == 0) ? 1.0 : chardelay::kStereoDecorr;

        if (character_type_ == Character::bbd) {
            const double depth =
                chardelay::interpolate_knots(chardelay::kBbdAxis, chardelay::kBbdFlutterDepth, character_amount);
            target *= 1.0 + depth * decorrelation * std::sin(2.0 * chardelay::kPi * bbd_flutter_phase_);
        }

        const double slewed = time_slew_[channel].process(target);
        const double modulation =
            mod_depth_ * decorrelation * std::sin(2.0 * chardelay::kPi * lfo_phase_);
        return slewed * (1.0 + modulation);
    }

    double process_character(ChannelState& channel, double x, double time_ms,
                             double character_amount, double decorrelation) noexcept {
        const double base_samples = time_ms * 0.001 * sample_rate_;
        double y = 0.0;
        switch (character_type_) {
            case Character::clean: {
                y = read_line(channel, x, base_samples, base_samples, 0.0);
                y = channel.loop_highpass.highpass(y);
                if (!clean_lowpass_bypassed_) y = channel.loop_lowpass.lowpass(y);
                break;
            }
            case Character::diffusion: {
                y = read_line(channel, x, base_samples, base_samples, 0.0);
                y = channel.diffusion.process(y, decorrelation);
                channel.diffusion.tick_modulation();
                y = channel.loop_highpass.highpass(y);
                y = channel.loop_lowpass.lowpass(y);
                break;
            }
            case Character::tape: {
                channel.instability.tick();
                const double wow =
                    chardelay::interpolate_knots(chardelay::kTapeAxis, chardelay::kTapeWowDepthMs, character_amount);
                const double flutter = chardelay::interpolate_knots(
                    chardelay::kTapeAxis, chardelay::kTapeFlutterDepthMs, character_amount);
                const double offset_samples =
                    channel.instability.offset_ms(wow, flutter) * 0.001 * sample_rate_;

                if (physical_tape()) {
                    // Fold the tier's in-loop group delay (the hysteresis
                    // oversampler plus both halves of the loss cascade) out of
                    // the line, so the echo lands where the time knob says and
                    // latency_samples() can stay 0. Below that floor the tier
                    // simply cannot deliver a shorter delay — a few hundred
                    // microseconds at typical rates — and the line clamps.
                    const double folded =
                        std::max(base_samples - channel.tape_physical.in_loop_delay_samples(), 1.0);
                    const double pre = channel.tape_physical.pre_process(x);
                    const double delayed =
                        read_line(channel, pre, folded + offset_samples, folded, offset_samples);
                    y = channel.tape_physical.post_process(delayed);
                } else {
                    const double pre = channel.tape.pre_process(x);
                    const double delayed = read_line(channel, pre, base_samples + offset_samples,
                                                     base_samples, offset_samples);
                    y = channel.tape.post_process(delayed);
                }
                break;
            }
            case Character::bbd: {
                // Clock-domain characters own their own line, so reverse runs
                // at the character's INPUT instead of replacing the line. The
                // segment carries the requested time and the line is shortened
                // to kReverseLineMs so the two delays sum to about the
                // requested time — see that constant for why the alternative
                // (both at full time) is unacceptable.
                const double v = reverse_on_
                                     ? channel.reverse.process(x, base_samples, 0.0)
                                     : x;
                y = channel.bbd.process(v);
                break;
            }
            case Character::vintage_digital: {
                const double v = reverse_on_
                                     ? channel.reverse.process(x, base_samples, 0.0)
                                     : x;
                const double line_ms =
                    reverse_on_ ? chardelay::kReverseLineMs : time_ms;
                y = channel.vintage.process(v, line_ms * 0.001);
                break;
            }
        }

        // Cross-character diffusion: smear ANY character's repeat through the
        // diffusion network by the smoothed amount, in-loop (post-line,
        // pre-saturator) — the same placement the diffusion character uses, so
        // repeats melt into a cloud as they recirculate. Dry/wet crossfade so 0
        // is the character untouched. Skipped for the diffusion character, which
        // already runs the network in its own branch above (avoids a double pass).
        if (diffusion_amount_value_ > 0.0 && character_type_ != Character::diffusion) {
            const double d = channel.diffusion.process(y, decorrelation);
            channel.diffusion.tick_modulation();
            y = y + diffusion_amount_value_ * (d - y);
        }
        return y;
    }

    /// Forward read from the shared line, or a reversed segment when reverse is
    /// engaged. `segment_samples` is the reverse segment length and
    /// `offset_samples` the instability displacement, kept separate so tape's
    /// wow and flutter keep modulating the read head when the transport runs
    /// backwards.
    double read_line(ChannelState& channel, double x, double delay_samples,
                     double segment_samples, double offset_samples) noexcept {
        // The forward line keeps being written even while reverse is engaged.
        // It costs one store per sample and it is what stops toggling reverse
        // OFF from replaying whatever fragment happened to be sitting in the
        // buffer from before reverse was switched on — which, at a two-second
        // maximum delay, can be a minute-old fragment of a different take.
        channel.line.push(x);
        if (reverse_on_) return channel.reverse.process(x, segment_samples, offset_samples);
        return channel.line.read(delay_samples);
    }

    std::array<ChannelState, 2> channels_{};
    std::array<chardelay::Smoother, 2> time_slew_{};
    chardelay::Smoother feedback_;
    chardelay::Smoother crossfeed_;
    chardelay::Smoother duck_;
    chardelay::Smoother freeze_;
    chardelay::Smoother character_;
    chardelay::Smoother diffusion_amount_;
    chardelay::EnvelopeFollower duck_follower_;

    Character character_type_ = Character::clean;
    TapeTier tape_tier_ = TapeTier::standard;

    double sample_rate_ = 48000.0;
    double time_ms_ = 350.0;
    double time_offset_ = 1.0;
    double right_time_ms_ = 350.0;
    bool right_time_absolute_ = false;
    double feedback_target_ = 0.35;
    double crossfeed_target_ = 0.0;
    double character_target_ = 0.5;
    double diffusion_amount_target_ = 0.0;
    double diffusion_amount_value_ = 0.0;   // smoothed once per sample in process()
    double duck_target_ = 0.0;
    double mod_rate_hz_ = 0.5;
    double mod_depth_ = 0.0;
    double tape_speed_ips_ = 7.5;
    bool freeze_on_ = false;
    bool reverse_on_ = false;

    // Player-facing in-loop tone. Defaults are fully open, which bypasses.
    double loop_low_cut_hz_ = chardelay::kLoopToneHpMinHz;
    double loop_low_cut_q_ = 0.707;
    double loop_high_cut_hz_ = chardelay::kLoopToneLpMaxHz;
    double loop_high_cut_q_ = 0.707;
    bool loop_tone_active_ = false;
    bool clean_lowpass_bypassed_ = true;

    double lfo_phase_ = 0.0;
    double bbd_flutter_phase_ = 0.0;
    double duck_gain_ = 1.0;
    double duck_gain_step_ = 0.0;
    double duck_peak_ = 0.0;
    int control_counter_ = 0;
};

using CharacterDelay = CharacterDelayT<float>;

}  // namespace pulp::signal
