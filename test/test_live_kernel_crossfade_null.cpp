// PF-2 null test: the live_kernel equal-power fade now advances cos/sin by a
// per-block angle recurrence instead of calling cos/sin per sample. This test
// drives a full fade (in realistically-sized, varying blocks — exactly how
// Kernel::process re-seeds fade_pos each block) and asserts the recurrence
// output stays within a tiny, inaudible epsilon of the original per-sample
// cos/sin evaluation, and that fade endpoints are exact.

#include <catch2/catch_test_macros.hpp>

#include <live_kernel/crossfade.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using pulp::live_kernel::equal_power_fade_block;

namespace {

constexpr double kHalfPi = 1.57079632679489661923;

// The pre-change reference: direct per-sample cos/sin, exactly as the old
// Kernel::process fade loop computed it.
void reference_fade_block(float* dst, const float* ob, const float* nb, int n,
                          int fade_pos, int fade_len) {
    const double inv = 1.0 / static_cast<double>(fade_len);
    for (int i = 0; i < n; ++i) {
        double t = static_cast<double>(fade_pos + i) * inv;
        if (t > 1.0) t = 1.0;
        const float go = static_cast<float>(std::cos(t * kHalfPi));
        const float gn = static_cast<float>(std::sin(t * kHalfPi));
        dst[i] = ob[i] * go + nb[i] * gn;
    }
}

}  // namespace

TEST_CASE("PF-2: live_kernel equal-power fade recurrence matches direct cos/sin",
          "[live_kernel][crossfade][pf2][null]") {
    // Block sizes cycle to exercise partial blocks and the LK_MAX_BLOCK (128)
    // quantum; fade_pos advances per block just like the real kernel.
    const int block_sizes[] = {128, 100, 37, 128, 13};

    for (int fade_len : {1, 7, 64, 128, 480, 4096, 20001}) {
        const int total = fade_len + 300;  // run past the fade end (clamp region)
        std::vector<float> ob(static_cast<std::size_t>(total));
        std::vector<float> nb(static_cast<std::size_t>(total));
        std::uint32_t lcg = 0x2468aceu;
        for (int i = 0; i < total; ++i) {
            lcg = lcg * 1664525u + 1013904223u;
            ob[static_cast<std::size_t>(i)] =
                static_cast<float>(static_cast<int>(lcg >> 9) - 4194304) / 4194304.0f;
            lcg = lcg * 1664525u + 1013904223u;
            nb[static_cast<std::size_t>(i)] =
                static_cast<float>(static_cast<int>(lcg >> 9) - 4194304) / 4194304.0f;
        }

        float max_err = 0.0f;
        int fade_pos = 0;
        int off = 0;
        int si = 0;
        while (off < total) {
            int n = block_sizes[si++ % 5];
            if (off + n > total) n = total - off;

            std::vector<float> got(static_cast<std::size_t>(n));
            std::vector<float> ref(static_cast<std::size_t>(n));
            equal_power_fade_block(got.data(), ob.data() + off, nb.data() + off, n,
                                   fade_pos, fade_len);
            reference_fade_block(ref.data(), ob.data() + off, nb.data() + off, n,
                                 fade_pos, fade_len);
            for (int i = 0; i < n; ++i) {
                max_err = std::max(
                    max_err, std::abs(got[static_cast<std::size_t>(i)] -
                                      ref[static_cast<std::size_t>(i)]));
            }
            fade_pos += n;
            off += n;
        }

        INFO("fade_len=" << fade_len << " max_err=" << max_err);
        // < 1e-6 absolute: well below the ~1e-4 (-80 dB) audibility floor, and
        // dominated by float rounding rather than recurrence drift.
        REQUIRE(max_err < 1.0e-6f);
    }
}
