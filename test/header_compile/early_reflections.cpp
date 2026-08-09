#include <pulp/signal/early_reflections.hpp>

void early_reflections_header_self_containment() {
    pulp::signal::EarlyReflections reflections;
    (void)reflections.configure(std::span<const pulp::signal::EarlyReflections::Tap>{});
    (void)reflections.prepare(48000.0, 100.0);
    reflections.reset();
}
