// A consumer that wants filter math and a musical clock, and nothing else.
//
// The point of this program is what it does NOT include. It uses one header
// from core/signal and one from core/timebase, so the link closure it acquires
// is the honest cost of adopting Pulp's DSP vocabulary in an outside project.
// Keeping it real work rather than a compile-only stub matters: an executable
// that is never linked cannot be asked what its link line contained.

#include <pulp/signal/biquad.hpp>
#include <pulp/timebase/tick.hpp>

#include <cmath>
#include <cstdio>

int main() {
    pulp::signal::Biquad filter;
    filter.set_coefficients(pulp::signal::Biquad::Type::lowpass,
                            1000.0f, 0.707f, 48000.0f);

    if (!pulp::signal::biquad_is_stable(filter.coefficients())) {
        std::fprintf(stderr, "designed biquad is unstable\n");
        return 1;
    }

    // Drive an impulse through and require the tail to decay. A low-pass fed
    // an impulse must settle; a filter that was never actually configured
    // returns the input unchanged and fails this.
    float peak_tail = 0.0f;
    for (int n = 0; n < 512; ++n) {
        const float out = filter.process(n == 0 ? 1.0f : 0.0f);
        if (!std::isfinite(out)) {
            std::fprintf(stderr, "filter produced a non-finite sample\n");
            return 2;
        }
        if (n > 256 && std::fabs(out) > peak_tail) {
            peak_tail = std::fabs(out);
        }
    }
    if (!(peak_tail < 1.0e-3f)) {
        std::fprintf(stderr, "filter tail did not decay (%g)\n",
                     static_cast<double>(peak_tail));
        return 3;
    }

    // The timebase side: saturating tick arithmetic over the musical grid.
    using namespace pulp::timebase;
    const TickPosition start{0};
    const TickDuration bar{kTicksPerQuarter * 4};
    const TickPosition second_bar = start + bar;
    if (second_bar.value != kTicksPerQuarter * 4) {
        std::fprintf(stderr, "tick arithmetic disagreed\n");
        return 4;
    }
    if ((second_bar - start).value != bar.value) {
        std::fprintf(stderr, "tick difference disagreed\n");
        return 5;
    }

    std::printf("minimal consumer ok\n");
    return 0;
}
