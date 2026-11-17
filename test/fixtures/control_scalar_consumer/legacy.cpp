// Installed-header consumer written against the deprecated indexed_control.hpp
// spellings. It must keep compiling, and each spelling must return exactly
// the canonical result. Returns non-zero when a result differs.
#include <pulp/signal/indexed_control.hpp>

#include <cstdio>
#include <cstring>
#include <limits>

namespace sig = pulp::signal;

namespace {
bool same_bits(float a, float b) {
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}
} // namespace

int main() {
    using L = std::numeric_limits<float>;
    const float values[] = {L::quiet_NaN(), L::infinity(), -L::infinity(), 0.0f, -0.0f, 1.0f, -3.0f,
                            L::max(),       L::lowest(),   L::denorm_min()};
    int failures = 0;
    for (const float a : values) {
        failures += same_bits(sig::finite_control_sample(a), sig::finite_control(a)) ? 0 : 1;
        for (const float b : values) {
            failures += same_bits(sig::control_minimum(a, b), sig::control_min(a, b)) ? 0 : 1;
            failures += same_bits(sig::control_maximum(a, b), sig::control_max(a, b)) ? 0 : 1;
            failures +=
                same_bits(sig::control_safe_divide(a, b), sig::safe_control_divide(a, b)) ? 0 : 1;
        }
    }
    if (failures == 0)
        std::puts("control_scalar_legacy_consumer=ok");
    else
        std::fprintf(stderr, "legacy consumer: %d mismatches\n", failures);
    return failures == 0 ? 0 : 1;
}
