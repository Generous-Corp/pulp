#include <pulp/signal/signal.hpp>

#include <limits>

int main() {
    pulp::signal::MirroredHistoryBuffer<float> mirrored_history;
    if (!mirrored_history.prepare(4u))
        return 1;
    mirrored_history.push(1.0f);
    if (mirrored_history.prepare(std::numeric_limits<std::size_t>::max() / 2u + 1u))
        return 2;
    if (mirrored_history.capacity() != 4u || mirrored_history.window().back() != 1.0f)
        return 3;

    pulp::signal::FractionalDelayHistory fractional_history;
    if (!fractional_history.prepare(8u))
        return 4;
    pulp::signal::FractionalDelayLine fractional_delay;
    if (!fractional_delay.prepare(8u, pulp::signal::FractionalDelayMethod::lagrange3))
        return 5;
    pulp::signal::AudioMatrixMixer matrix;
    if (!matrix.prepare(16u))
        return 6;
    pulp::signal::PathLatencyAligner aligner;
    return aligner.prepare(2u, 1u, 8u, 16u) ? 0 : 7;
}
