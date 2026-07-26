#pragma once

#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/drum/voice.hpp>
#include <pulp/signal/svf.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace pulp::signal::drum {

/// A six-operator FM voice with the classic thirty-two algorithm set.
///
/// Six operators rather than the eight of `Fm8DrumVoice`, because six is what
/// makes this instrument the thing it is. The constraint is the character: with
/// only six operators an algorithm has to spend them, so the familiar routings
/// are each a specific compromise between how many partials reach the output
/// and how deeply they are modulated. A wider engine does not sound like this
/// one with more headroom — it sounds like a different instrument. `Fm8DrumVoice`
/// is the more capable engine; this is the more recognisable one.
///
/// The architecture is six operators, thirty-two fixed routings, one feedback
/// operator per routing, and a global pitch envelope. Envelope shaping is
/// `DecayEnvelopeT`, so an operator's contour is described in the same
/// milliseconds-and-T60 terms as every other voice here rather than in
/// device-specific rate and level units.
///
/// The routing table is transcribed from published descriptions of the
/// instrument's architecture. Three rules the documentation states about the
/// original algorithm set are asserted over every row, which is what makes the
/// transcription checkable rather than merely plausible:
///
///  * **Modulation only ever flows downward.** The set is laid out with the
///    highest-numbered operator at the top, so an operator is modulated only by
///    higher-numbered ones. This is the strongest of the three — it rejects any
///    row whose arrows point the wrong way, which is the easiest transcription
///    error to make and the hardest to hear.
///  * **At most one self-feedback loop per routing**, which is why
///    `feedback_op` is a single index rather than a mask.
///  * **Carrier outputs are scaled by the carrier count**, so routings with
///    different numbers of carriers arrive at comparable levels; the fully
///    additive routing is scaled by a sixth. `render_add` does this.
///
/// Alongside those, the structural invariants: at least one carrier, no
/// operator modulating itself through the mask, every index in range, and every
/// operator reachable from a carrier. Algorithm 8 is additionally pinned
/// against its documented topology as a worked example.
///
/// What none of that proves is that a given row *is* the routing that number
/// names. A row can satisfy every rule above and still be the wrong shape.
/// Anything depending on patch-level algorithm compatibility should verify the
/// full table against the operation manual's diagrams first.
///
/// Lineage: Chowning, JAES 21(7), 1973, for the synthesis technique.
///
/// RT contract: `prepare()` allocates nothing; every other method allocates
/// nothing and takes no locks.
class Fm6DrumVoice : public Voice {
public:
    static constexpr int operator_count = 6;
    static constexpr int algorithm_count = 32;

    /// One routing. `modulated_by[op]` is a bitmask of the operators feeding
    /// `op`; `carriers` is a bitmask of the operators that reach the output;
    /// `feedback_op` is the single operator whose output feeds its own phase.
    struct Algorithm {
        std::uint8_t modulated_by[operator_count];
        std::uint8_t carriers;
        std::uint8_t feedback_op;
    };

    // Bit i is operator i, zero-indexed (bit 0 is the instrument's OP1).
    static constexpr Algorithm algorithms[algorithm_count] = {
        {{0x02, 0, 0x08, 0x10, 0x20, 0}, 0x05, 5},  //  1 two stacks
        {{0x02, 0, 0x08, 0x10, 0x20, 0}, 0x05, 1},  //  2 as 1, feedback moved
        {{0x02, 0x04, 0, 0x10, 0x20, 0}, 0x09, 5},  //  3 three plus three
        {{0x02, 0x04, 0, 0x10, 0x20, 0}, 0x09, 3},  //  4 as 3, feedback moved
        {{0x02, 0, 0x08, 0, 0x20, 0}, 0x15, 5},     //  5 three pairs
        {{0x02, 0, 0x08, 0, 0x20, 0}, 0x15, 4},     //  6 as 5, feedback moved
        {{0x02, 0, 0x18, 0, 0x20, 0}, 0x05, 5},     //  7 pair plus branch
        {{0x02, 0, 0x18, 0, 0x20, 0}, 0x05, 3},     //  8 as 7, feedback moved
        {{0x02, 0, 0x18, 0, 0x20, 0}, 0x05, 1},     //  9 as 7, feedback moved
        {{0x02, 0x04, 0, 0x30, 0, 0}, 0x09, 2},     // 10 stack plus fan-in
        {{0x02, 0x04, 0, 0x30, 0, 0}, 0x09, 5},     // 11 as 10, feedback moved
        {{0x02, 0, 0x38, 0, 0, 0}, 0x05, 1},        // 12 pair plus triple fan-in
        {{0x02, 0, 0x38, 0, 0, 0}, 0x05, 5},        // 13 as 12, feedback moved
        {{0x02, 0, 0x18, 0x20, 0, 0}, 0x05, 5},     // 14 pair plus chain
        {{0x02, 0, 0x18, 0x20, 0, 0}, 0x05, 1},     // 15 as 14, feedback moved
        {{0x16, 0, 0x08, 0, 0x20, 0}, 0x01, 5},     // 16 single carrier, three arms
        {{0x16, 0, 0x08, 0, 0x20, 0}, 0x01, 1},     // 17 as 16, feedback moved
        {{0x0E, 0, 0, 0x10, 0x20, 0}, 0x01, 2},     // 18 single carrier, deep arm
        {{0x02, 0x04, 0, 0x20, 0x20, 0}, 0x19, 5},  // 19 stack plus shared modulator
        {{0x04, 0x04, 0, 0x30, 0, 0}, 0x0B, 2},     // 20 shared modulator, two carriers
        {{0x04, 0x04, 0, 0x20, 0x20, 0}, 0x1B, 2},  // 21 shared modulators, four carriers
        {{0x02, 0, 0x20, 0x20, 0x20, 0}, 0x1D, 5},  // 22 one modulator into three
        {{0x02, 0x04, 0x20, 0x20, 0x20, 0}, 0x1B, 5},  // 23 as 22, one arm folded in
        {{0x02, 0, 0x20, 0x20, 0x20, 0}, 0x1F, 5},  // 24 five carriers, one arm
        {{0, 0, 0x20, 0x20, 0x20, 0}, 0x1F, 5},     // 25 five carriers, one modulator
        {{0x02, 0x04, 0x30, 0x20, 0x20, 0}, 0x0B, 5},  // 26 chain plus fan-in
        {{0x02, 0x04, 0x30, 0x20, 0x20, 0}, 0x0B, 2},  // 27 as 26, feedback moved
        {{0x02, 0, 0x18, 0x20, 0, 0}, 0x25, 4},     // 28 chain with high carrier
        {{0x02, 0, 0x08, 0x20, 0x20, 0}, 0x17, 5},  // 29 four carriers
        {{0x02, 0, 0x18, 0x20, 0, 0}, 0x27, 4},     // 30 chain, four carriers
        {{0x02, 0, 0, 0, 0x20, 0}, 0x1F, 5},        // 31 five carriers, one pair
        {{0, 0, 0, 0, 0, 0}, 0x3F, 5},              // 32 fully additive
    };

    Fm6DrumVoice() {
        VelocityResponse r;
        r.level_db = 15.0f;
        r.brightness_octaves = 1.0f;
        set_velocity_response(r);
    }

    void set_algorithm(int index) {
        algorithm_ = std::clamp(index, 0, algorithm_count - 1);
    }
    int algorithm() const { return algorithm_; }

    void set_tune_hz(double hz) { tune_hz_ = std::clamp(hz, 20.0, 4000.0); }

    void set_operator_ratio(int op, double ratio) {
        if (op < 0 || op >= operator_count) return;
        ratios_[static_cast<std::size_t>(op)] = std::clamp(ratio, 0.1, 32.0);
    }

    void set_operator_level(int op, double level) {
        if (op < 0 || op >= operator_count) return;
        levels_[static_cast<std::size_t>(op)] = std::clamp(level, 0.0, 1.0);
    }

    void set_operator_decay_ms(int op, double ms) {
        if (op < 0 || op >= operator_count) return;
        decays_[static_cast<std::size_t>(op)] = std::clamp(ms, 1.0, 4000.0);
    }

    /// Depth of the routing's designated feedback operator, 0 to 1.
    void set_feedback(double amount) { feedback_ = std::clamp(amount, 0.0, 1.0); }

    /// Overall modulation depth, scaling every routed connection.
    void set_depth(double depth) { depth_ = std::clamp(depth, 0.0, 12.0); }

    /// Global pitch envelope: every operator's frequency is scaled together, so
    /// the spectrum keeps its shape while the whole voice sweeps.
    void set_pitch_sweep_octaves(double octaves) {
        pitch_sweep_oct_ = std::clamp(octaves, -6.0, 6.0);
    }
    void set_pitch_sweep_ms(double ms) { pitch_sweep_ms_ = std::clamp(ms, 0.5, 1000.0); }

    void set_formant_hz(double hz) { formant_hz_ = std::clamp(hz, 40.0, 18000.0); }
    void set_formant_q(double q) { formant_q_ = std::clamp(q, 0.5, 12.0); }

    OutputStage& output() { return output_; }

protected:
    void on_prepare(double sample_rate) override {
        for (auto& env : envelopes_) env.set_sample_rate(sample_rate);
        pitch_env_.set_sample_rate(sample_rate);
        formant_.set_sample_rate(static_cast<float>(sample_rate));
        formant_.set_mode(Svf::Mode::bandpass);
        output_.prepare(sample_rate);
    }

    void on_reset() override {
        for (auto& env : envelopes_) env.reset();
        pitch_env_.reset();
        phases_.fill(0.0);
        previous_.fill(0.0);
        formant_.reset();
        output_.reset();
    }

    void on_note_on(float velocity) override {
        output_.reset();
        const auto& response = velocity_response();
        velocity_gain_ = response.gain(velocity);
        applied_depth_ =
            depth_ * static_cast<double>(response.brightness_scale(velocity));

        phases_.fill(0.0);
        previous_.fill(0.0);
        formant_.set_frequency(
            static_cast<float>(std::min(formant_hz_, 0.49 * sample_rate())));
        formant_.set_resonance(static_cast<float>(formant_q_));

        pitch_env_.set_attack_ms(0.0);
        pitch_env_.set_decay_time_constant_ms(pitch_sweep_ms_);
        pitch_env_.trigger();

        for (std::size_t op = 0; op < operator_count; ++op) {
            envelopes_[op].set_attack_ms(0.2);
            envelopes_[op].set_decay_t60_ms(decays_[op]);
            envelopes_[op].trigger();
        }
    }

    bool on_is_active() const override {
        for (const auto& env : envelopes_) {
            if (env.is_active()) return true;
        }
        return output_.has_tail();
    }

    void render_add(float* out, int num_samples) override {
        const Algorithm& alg = algorithms[static_cast<std::size_t>(algorithm_)];
        const auto feedback_op = static_cast<std::size_t>(alg.feedback_op);

        for (int i = 0; i < num_samples; ++i) {
            const double sweep = std::exp2(pitch_sweep_oct_ * pitch_env_.process());
            std::array<double, operator_count> current{};

            for (std::size_t op = 0; op < operator_count; ++op) {
                // Every modulation input is read one sample late, uniformly, so
                // the routing table needs no ordering analysis and a routing may
                // contain cycles. Same trick as the eight-operator engine.
                double modulation = 0.0;
                const std::uint8_t mask = alg.modulated_by[op];
                for (std::size_t src = 0; src < operator_count; ++src) {
                    if (mask & (1u << src)) modulation += previous_[src];
                }
                modulation *= applied_depth_;
                if (op == feedback_op) {
                    modulation += feedback_ * kFeedbackDepth * previous_[op];
                }

                const double hz =
                    std::min(tune_hz_ * ratios_[op] * sweep, 0.49 * sample_rate());
                phases_[op] += hz / sample_rate();
                if (phases_[op] >= 1.0) phases_[op] -= std::floor(phases_[op]);

                current[op] =
                    std::sin(2.0 * 3.14159265358979323846 * phases_[op] + modulation) *
                    envelopes_[op].process() * levels_[op];
            }

            double summed = 0.0;
            int carriers = 0;
            for (std::size_t op = 0; op < operator_count; ++op) {
                if (alg.carriers & (1u << op)) {
                    summed += current[op];
                    ++carriers;
                }
            }
            if (carriers > 1) summed /= static_cast<double>(carriers);

            previous_ = current;

            const double shaped = formant_.process(static_cast<float>(summed));
            out[i] += static_cast<float>(
                output_.process(static_cast<float>(shaped)) * velocity_gain_);
        }
    }

private:
    static constexpr double kFeedbackDepth = 6.0;

    int algorithm_ = 4;
    double tune_hz_ = 110.0;
    double depth_ = 3.0;
    double feedback_ = 0.0;
    double pitch_sweep_oct_ = 0.0;
    double pitch_sweep_ms_ = 40.0;
    double formant_hz_ = 3000.0;
    double formant_q_ = 0.9;

    std::array<double, operator_count> ratios_ = {1.0, 1.0, 2.0, 3.0, 4.0, 7.0};
    std::array<double, operator_count> levels_ = {1.0, 0.8, 0.7, 0.6, 0.5, 0.4};
    std::array<double, operator_count> decays_ = {400.0, 260.0, 180.0,
                                                   120.0, 80.0,  50.0};

    std::array<DecayEnvelope64, operator_count> envelopes_;
    DecayEnvelope64 pitch_env_;
    std::array<double, operator_count> phases_{};
    std::array<double, operator_count> previous_{};
    Svf formant_;
    OutputStage output_;

    double velocity_gain_ = 1.0;
    double applied_depth_ = 3.0;
};

}  // namespace pulp::signal::drum
