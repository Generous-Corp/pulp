#include <pulp/signal/unit_delay.hpp>

int main() {
    pulp::signal::UnitDelay delay;
    if (delay.process(1.0f) != 0.0f)
        return 1;
    if (delay.process(0.0f) != 1.0f)
        return 2;
    delay.reset();
    if (delay.publish() != 0.0f)
        return 3;

    delay.commit(-0.25f);
    return delay.publish() == -0.25f ? 0 : 4;
}
