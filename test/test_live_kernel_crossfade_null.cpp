// SF-2 crossfade unification — live_kernel law-parity test.
//
// This is an INTENTIONAL, documented behavior change (not a null test in the
// bit-preserving sense). Before SF-2 the live_kernel structural-swap fade used a
// LINEAR theta (theta = t * pi/2, no smoothstep) while its comment claimed to
// match the native live plugin swap's EqualPower law. That was a provable
// divergence: the native law shapes t through a smoothstep ramp first, so
// mid-fade the two curves differ and the kernel would fail a bit-exact match
// against native.
//
// SF-2 routes the kernel fade through the shared signal::crossfade utility — the
// SAME smoothstep-shaped equal-power split that signal::TransitionMixer (float,
// EqualPower) computes on the native live-swap path. This test proves the fix:
// the kernel's per-sample old/new gains are now BIT-IDENTICAL to the native
// TransitionMixer, across realistically-sized varying blocks (exactly how
// Kernel::process re-seeds fade_pos each block).

#include <catch2/catch_test_macros.hpp>

#include <live_kernel/crossfade.hpp>
#include <pulp/signal/transition_mixer.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using pulp::live_kernel::equal_power_fade_block;
using pulp::signal::TransitionCurve;
using pulp::signal::TransitionMixer;

namespace {

// The native reference: the shared TransitionMixer (float, EqualPower) — the
// exact mixer the native live plugin swap (crossfade_plugin_slot) blends with.
// gains_at() is a pure function of the absolute fade position, so we can query it
// per sample without advancing.
void native_fade_block(float* dst, const float* ob, const float* nb, int n,
                       int fade_pos, int fade_len) {
    TransitionMixer mixer;
    mixer.configure(static_cast<std::size_t>(fade_len), TransitionCurve::EqualPower);
    for (int i = 0; i < n; ++i) {
        float go = 0.0f;
        float gn = 0.0f;
        mixer.gains_at(static_cast<std::size_t>(fade_pos + i), go, gn);
        dst[i] = ob[i] * go + nb[i] * gn;
    }
}

}  // namespace

TEST_CASE("SF-2: live_kernel equal-power fade now matches the native TransitionMixer law",
          "[live_kernel][crossfade][sf2]") {
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
            native_fade_block(ref.data(), ob.data() + off, nb.data() + off, n,
                              fade_pos, fade_len);
            for (int i = 0; i < n; ++i) {
                INFO("fade_len=" << fade_len << " fade_pos=" << (fade_pos + i));
                // Bit-exact: the kernel now runs the identical shared law.
                REQUIRE(got[static_cast<std::size_t>(i)] ==
                        ref[static_cast<std::size_t>(i)]);
            }
            fade_pos += n;
            off += n;
        }
    }
}
