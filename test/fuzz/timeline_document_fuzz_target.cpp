/// libFuzzer entry point over the untrusted-document oracles.
///
/// This target is built only when `PULP_ENABLE_FUZZING=ON`, because
/// `-fsanitize=fuzzer` needs a compiler runtime Apple's clang does not ship —
/// on macOS the toolchain must be an LLVM that provides
/// `libclang_rt.fuzzer_osx.a`. The oracles themselves are toolchain-independent
/// and are also driven by a deterministic replay in the normal test suite, so
/// the coverage-guided lane is an amplifier for the properties rather than the
/// only place they are checked.

#include "timeline_document_oracle.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

/// Coverage-guided exploration runs at a rate where the per-axis tightening
/// sweep — one extra parse per populated axis — would dominate the input rate.
/// It is sampled rather than dropped: every input still carries the crash,
/// determinism, admission, divergence, and amplification oracles.
constexpr std::uint64_t kEnforcementSweepPeriod = 64;

std::uint64_t g_iteration = 0;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace pulp::test::timeline_fuzz;

    const std::string_view input(reinterpret_cast<const char*>(data), size);

    InspectOptions options;
    options.check_quota_enforcement = (g_iteration % kEnforcementSweepPeriod) == 0;
    ++g_iteration;

    if (const auto finding = inspect_all(input, options)) {
        std::fprintf(stderr, "pulp-timeline-fuzz: %s\n", format_finding(finding).c_str());
        // Aborting is what makes a property violation a libFuzzer crash: the
        // engine only preserves and minimizes an input when the process dies,
        // so a finding that merely returned would be explored and discarded.
        std::abort();
    }
    return 0;
}
