#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/signal/trigger.hpp>

#include <cmath>
#include <cstddef>
#include <iostream>

int main() {
    using namespace pulp::signal;
    constexpr double sr = 48000.0;
    TriggerDetect trigger;
    trigger.prepare(sr);
    trigger.set_threshold(0.5f);
    trigger.set_refractory_ms(0.1);
    DecayEnvelope envelope;
    envelope.set_sample_rate(sr);
    envelope.set_attack_ms(0.0);
    envelope.set_hold_ms(0.0);
    envelope.set_decay_t60_ms(12.0);
    TptFilter filter;
    filter.prepare(static_cast<float>(sr));
    filter.set_cutoff(1200.0f);

    int first_trigger = -1;
    int second_trigger = -1;
    float feedback = 0.0f;
    for (int n = 0; n < 96; ++n) {
        const float onset = n == 8 || n == 40 ? 1.0f : 0.0f;
        const bool fired = trigger.process_signal(onset);
        if (fired) {
            envelope.trigger(1.0f);
            if (first_trigger < 0)
                first_trigger = n;
            else if (second_trigger < 0)
                second_trigger = n;
        }
        const float env = envelope.process();
        const float mod = 900.0f * std::sin(2.0 * M_PI * 3.0 * n / sr);
        filter.set_cutoff(1200.0f + mod);
        const float filtered = filter.process_lowpass(env + 0.25f * feedback);
        feedback = filtered;
    }
    const bool positive = first_trigger == 8 && second_trigger == 40 && std::isfinite(feedback);
    // Planted negative: an exact threshold sample is not a strict crossing.
    TriggerDetect negative;
    negative.prepare(sr);
    negative.set_threshold(0.5f);
    const bool planted_negative = !negative.process_signal(0.5f);
    std::cout << "fixture=g5-events sr=48000 first_trigger=" << first_trigger
              << " second_trigger=" << second_trigger << " positive=" << positive
              << " planted_negative=" << planted_negative << " final_feedback=" << feedback << "\n";
    return positive && planted_negative ? 0 : 1;
}
