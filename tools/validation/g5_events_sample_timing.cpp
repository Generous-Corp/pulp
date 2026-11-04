#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/indexed_control.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/signal/trigger.hpp>

#include <cmath>
#include <cstddef>
#include <iostream>
#include <array>
#include <limits>
#include <vector>

namespace {
struct Receipt {
    std::vector<int> triggers;
    std::vector<float> output;
    float feedback = 0.0f;
};

Receipt render(const std::vector<int>& partitions) {
    using namespace pulp::signal;
    constexpr double sr = 48000.0;
    constexpr std::array<float, 4> cutoff_wave{0.0f, 0.5f, 1.0f, 0.0f};
    IndexedControl indexed;
    indexed.set_table(cutoff_wave);
    MapClip map;
    if (!map.configure(0.0f, 1.0f, 1200.0f, 2100.0f))
        return {};
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

    Receipt receipt;
    receipt.output.reserve(96);
    int absolute_sample = 0;
    for (int block_size : partitions) {
        for (int offset = 0; offset < block_size; ++offset, ++absolute_sample) {
            const float onset = absolute_sample == 8 || absolute_sample == 40 ? 1.0f : 0.0f;
            if (trigger.process_signal(onset)) {
                receipt.triggers.push_back(absolute_sample);
                envelope.trigger(1.0f);
            }
            // External time index pauses at 24 and reverses after 64. The
            // caller, not the waveform reader, owns this chronology.
            const double time_index = absolute_sample < 24   ? absolute_sample / 16.0
                                      : absolute_sample < 64 ? 1.5
                                                             : 1.5 - (absolute_sample - 64) / 32.0;
            const float modulation = indexed.process(time_index);
            filter.set_cutoff(map.process(modulation));
            receipt.feedback =
                filter.process_lowpass(envelope.process() + 0.25f * receipt.feedback);
            receipt.output.push_back(receipt.feedback);
        }
    }
    return receipt;
}
} // namespace

int main() {
    using namespace pulp::signal;
    const auto whole = render({96});
    const auto split = render({8, 1, 15, 16, 24, 32});
    const bool positive = whole.triggers == std::vector<int>{8, 40} &&
                          whole.triggers == split.triggers && whole.output == split.output &&
                          whole.output.size() == 96 && std::isfinite(whole.feedback);
    // Planted negative: an exact threshold sample is not a strict crossing.
    TriggerDetect negative;
    negative.prepare(48000.0);
    negative.set_threshold(0.5f);
    MapClip map;
    const bool bad_range_rejected = !map.configure(1.0f, 1.0f, 0.0f, 1.0f);
    const bool bad_range_preserved = map.process(0.5f) == 0.5f;
    IndexedControl indexed;
    const bool missing_table_zero = indexed.process(12.0) == 0.0f;
    constexpr std::array<float, 2> waveform{0.0f, 1.0f};
    indexed.set_table(waveform);
    const bool indexed_controls = indexed.process(0.5) == 0.5f && indexed.process(-1.0) == 0.0f &&
                                  indexed.process(2.0) == 1.0f &&
                                  indexed.process(std::numeric_limits<double>::quiet_NaN()) == 0.0f;
    const bool planted_negative = !negative.process_signal(0.5f) && bad_range_rejected &&
                                  bad_range_preserved && missing_table_zero && indexed_controls;
    const int first_trigger = whole.triggers.size() > 0 ? whole.triggers[0] : -1;
    const int second_trigger = whole.triggers.size() > 1 ? whole.triggers[1] : -1;
    std::cout << "fixture=g5-events sr=48000 first_trigger=" << first_trigger
              << " second_trigger=" << second_trigger << " positive=" << positive
              << " planted_negative=" << planted_negative
              << " partition_invariant=" << (whole.output == split.output)
              << " indexed_control=" << indexed_controls << " final_feedback=" << whole.feedback
              << "\n";
    return positive && planted_negative ? 0 : 1;
}
