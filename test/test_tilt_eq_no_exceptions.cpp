#include <pulp/signal/tilt_eq.hpp>

#include <array>

int main() {
    pulp::signal::TiltEqT<float, 1> eq;
    if (!eq.prepare(48000.0))
        return 1;
    if (!eq.set_config({1000.0, 3.0}))
        return 2;
    std::array<float, 8> samples{};
    samples[0] = 1.0f;
    return eq.process_block(samples.data(), samples.size()) ? 0 : 3;
}
