#include <pulp/signal/signal.hpp>

#include <limits>

int main() {
    pulp::signal::MirroredHistoryBuffer<float> history;
    if (!history.prepare(4u))
        return 1;
    history.push(1.0f);
    if (history.prepare(std::numeric_limits<std::size_t>::max() / 2u + 1u))
        return 2;
    return history.capacity() == 4u && history.window().back() == 1.0f ? 0 : 3;
}
