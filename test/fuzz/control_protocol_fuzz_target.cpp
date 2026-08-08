#include "control_protocol_fuzz_oracle.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    if (const auto finding = pulp::test::control_protocol_fuzz::inspect(input)) {
        const auto formatted = pulp::test::control_protocol_fuzz::format_finding(finding);
        std::fprintf(stderr, "pulp-control-protocol-fuzz: %s\n", formatted.c_str());
        std::abort();
    }
    return 0;
}
