#pragma once

/// @file denormal_tail.hpp
/// Decaying-tail fixture for recursive-filter denormal tests.
///
/// Feeds a unit impulse followed by silence through a block-processing
/// callable and reports how many output samples were subnormal and where the
/// output last differed from exact zero. Those two numbers are what a missing
/// or too-sparse denormal snap changes: an unsnapped float recursion with a
/// pole near |z| = 1 rounds into a subnormal limit cycle that never reaches
/// zero, so every later sample is subnormal (and, on x86 without FTZ, slow).
///
/// The counts only mean something while subnormals are representable, so
/// callers must check `flush_to_zero_active()` first and skip when the thread
/// flushes them.

#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace pulp::test::denormal_tail {

/// True when the calling thread flushes subnormal results to zero.
inline bool flush_to_zero_active() {
    volatile float tiny = 1e-40f;
    volatile float one = 1.0f;
    return tiny * one == 0.0f;
}

struct TailStats {
    std::size_t subnormal_outputs = 0;
    // Index one past the last non-zero output; 0 when every output is zero.
    std::size_t nonzero_end = 0;
};

/// Run an impulse of @p total samples through @p process_block in blocks of
/// @p block samples (the final block may be shorter).
template <typename ProcessBlock>
TailStats impulse_tail(ProcessBlock&& process_block, std::size_t total, std::size_t block) {
    std::vector<float> buffer(total, 0.0f);
    buffer[0] = 1.0f;
    for (std::size_t pos = 0; pos < total; pos += block)
        process_block(buffer.data() + pos, static_cast<int>(std::min(block, total - pos)));

    TailStats stats;
    for (std::size_t i = 0; i < total; ++i) {
        if (pulp::signal::is_denormal(buffer[i]))
            ++stats.subnormal_outputs;
        if (buffer[i] != 0.0f)
            stats.nonzero_end = i + 1;
    }
    return stats;
}

} // namespace pulp::test::denormal_tail
