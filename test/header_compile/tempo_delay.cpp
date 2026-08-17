#include <pulp/signal/tempo_delay.hpp>

void tempo_delay_header_self_containment() {
    pulp::signal::TempoDelayTime time;
    (void)time.prepare(48000.0, 96000.0);
    (void)time.set_tempo(pulp::timebase::BeatDivision::QuarterDotted, 120.0);
    (void)time.next();
}
