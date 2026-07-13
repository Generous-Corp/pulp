// Snap-DISABLED reference generator for the MF-3 denormal null test.
//
// This binary is compiled end to end with PULP_DSP_ENABLE_SNAP_TO_ZERO=0, so
// every header-inline filter it instantiates is the genuine pre-guard code. It
// links no translation unit built with the shipping (snap-enabled) default —
// that separation is the whole point, see denormal_null_reference.hpp.
//
// Usage: denormal-null-refgen <out-blob-path>

#define PULP_DSP_ENABLE_SNAP_TO_ZERO 0

#include "denormal_null_reference.hpp"

#include <pulp/signal/scoped_flush_denormals.hpp>

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <out-blob-path>\n", argv[0]);
        return 2;
    }

    static_assert(PULP_DSP_ENABLE_SNAP_TO_ZERO == 0,
                  "the reference generator must be built with snap disabled");

    const denormal_null::AllOutputs outputs = denormal_null::render_all();
    const denormal_null::TailReport tail = denormal_null::render_tails();

    // Report THIS process's FP mode: hardware FTZ here is what would make a
    // subnormal physically unrepresentable in the reference tail.
    const bool flushed = pulp::signal::denormals_are_flushed();

    if (!denormal_null::write_reference(argv[1], outputs, tail, flushed)) {
        std::fprintf(stderr, "refgen: could not write %s\n", argv[1]);
        return 1;
    }
    return 0;
}
