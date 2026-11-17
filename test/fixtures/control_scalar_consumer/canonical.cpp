// Installed-header consumer written against the canonical scalar Control
// names. Returns non-zero when a pinned value differs.
#include <pulp/signal/control_math.hpp>
#include <pulp/signal/indexed_control.hpp>

#include <cmath>
#include <cstdio>
#include <limits>

namespace sig = pulp::signal;

int main() {
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    int failures = 0;
    const auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "canonical consumer: %s\n", what);
            ++failures;
        }
    };
    expect(sig::finite_control(nan) == 0.0f, "finite_control(NaN)");
    expect(sig::control_min(inf, 2.0f) == 0.0f, "control_min(inf, 2)");
    expect(sig::control_max(-inf, -2.0f) == 0.0f, "control_max(-inf, -2)");
    expect(std::signbit(sig::control_min(-0.0f, 0.0f)), "control_min(-0, +0)");
    expect(sig::safe_control_divide(1.0f, 0.0f) == 0.0f, "safe_control_divide(1, 0)");
    expect(sig::safe_control_divide(std::numeric_limits<float>::max(), 0.5f) ==
               std::numeric_limits<float>::max(),
           "safe_control_divide saturation");
    expect(sig::control_clamp(5.0f, 1.0f, -1.0f) == 1.0f, "control_clamp reversed bounds");
    expect(sig::integrator_step(0.75f, 4.0f, -1.0f, 1.0f) == 1.0f, "integrator_step saturation");
    sig::ControlIntegrator integrator;
    expect(integrator.configure(-1.0f, 1.0f, 0.25f), "configure");
    expect(integrator.process(inf) == 0.25f, "integrator non-finite increment");
    expect(!integrator.configure(1.0f, -1.0f), "reversed configure refused");
    integrator.reset();
    expect(integrator.value() == 0.25f, "reset");
    // Non-deprecated indexed utilities still compile under the strict flag.
    expect(sig::map_clip(0.5f, 0.0f, 1.0f, 10.0f, 20.0f) == 15.0f, "map_clip");
    sig::Slew slew;
    slew.set_rates(0.25f, 0.25f);
    expect(slew.process(nan) == 0.0f, "slew non-finite target");
    if (failures == 0)
        std::puts("control_scalar_canonical_consumer=ok");
    return failures == 0 ? 0 : 1;
}
