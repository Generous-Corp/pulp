#include "timeline_multitrack_arrangement.hpp"

#include <pulp/format/headless.hpp>

#include <array>
#include <cmath>
#include <vector>

int main() {
    pulp::format::HeadlessHost host(
        pulp::examples::timeline_phase1::create_timeline_multitrack_arrangement);
    host.prepare(48'000.0, 128, 2, 2);
    if (!host.valid())
        return 1;

    std::vector<float> left(128), right(128), input_left(128), input_right(128);
    std::array<float*, 2> output_ptrs{left.data(), right.data()};
    std::array<const float*, 2> input_ptrs{input_left.data(), input_right.data()};
    auto output = pulp::audio::BufferView<float>(output_ptrs.data(), 2, left.size());
    auto input = pulp::audio::BufferView<const float>(input_ptrs.data(), 2, input_left.size());
    host.process(output, input);

    const auto expected =
        static_cast<std::size_t>(pulp::examples::timeline_phase1::
                                     TimelineMultitrackArrangementProcessor::pdc_latency_samples);
    if (std::abs(left[expected] - 2.0f) > 1.0e-6f)
        return 1;
    for (std::size_t frame = 0; frame < expected; ++frame) {
        if (left[frame] != 0.0f || right[frame] != 0.0f)
            return 1;
    }
    return 0;
}
