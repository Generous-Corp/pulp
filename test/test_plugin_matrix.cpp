#include "test_matrix.hpp"
#include "pulp_gain.hpp"

// Test matrix exercises PulpGain across sample rates, buffer sizes, and edge cases.
// This pattern can be extended to all example plugins.

using namespace pulp;
using namespace pulp::test;

static format::ProcessorFactory gain_factory = examples::create_pulp_gain;

TEST_CASE("PulpGain matrix: silence produces finite output", "[matrix][gain]") {
    verify_silence_produces_finite(gain_factory);
}

TEST_CASE("PulpGain matrix: impulse produces finite output", "[matrix][gain]") {
    verify_impulse_produces_finite(gain_factory);
}

TEST_CASE("PulpGain matrix: DC offset stable", "[matrix][gain]") {
    verify_dc_offset_stable(gain_factory);
}

TEST_CASE("PulpGain matrix: state round-trip", "[matrix][gain]") {
    verify_state_round_trip(gain_factory);
}

TEST_CASE("PulpGain matrix: zero-length buffer", "[matrix][gain]") {
    verify_zero_length_buffer(gain_factory);
}

TEST_CASE("PulpGain matrix: sample rate sweep", "[matrix][gain]") {
    sweep_sample_rates_and_buffers(gain_factory,
        [](format::HeadlessHost& host, double, int bs) {
            // Process one block of silence — should not crash or produce NaN
            std::vector<float> in_l(static_cast<size_t>(bs), 0.0f);
            std::vector<float> in_r(static_cast<size_t>(bs), 0.0f);
            std::vector<float> out_l(static_cast<size_t>(bs));
            std::vector<float> out_r(static_cast<size_t>(bs));

            const float* in_ptrs[] = {in_l.data(), in_r.data()};
            float* out_ptrs[] = {out_l.data(), out_r.data()};
            audio::BufferView<const float> in_view(in_ptrs, 2, bs);
            audio::BufferView<float> out_view(out_ptrs, 2, bs);

            host.process(out_view, in_view);

            for (int i = 0; i < bs; ++i) {
                if (!std::isfinite(out_l[static_cast<size_t>(i)])) return false;
                if (!std::isfinite(out_r[static_cast<size_t>(i)])) return false;
            }
            return true;
        });
}

TEST_CASE("PulpGain matrix: parameter automation sweep", "[matrix][gain]") {
    format::HeadlessHost host(gain_factory);
    host.prepare(44100, 128);

    auto params = host.state().all_params();

    for (const auto& p : params) {
        // Sweep parameter from min to max in 10 steps
        for (int step = 0; step <= 10; ++step) {
            float norm = static_cast<float>(step) / 10.0f;
            host.state().set_normalized(p.id, norm);

            std::vector<float> in_l(128, 0.1f);
            std::vector<float> in_r(128, 0.1f);
            std::vector<float> out_l(128);
            std::vector<float> out_r(128);

            const float* in_ptrs[] = {in_l.data(), in_r.data()};
            float* out_ptrs[] = {out_l.data(), out_r.data()};
            audio::BufferView<const float> in_view(in_ptrs, 2, 128);
            audio::BufferView<float> out_view(out_ptrs, 2, 128);

            host.process(out_view, in_view);

            INFO("param=" << p.name << " norm=" << norm);
            for (int i = 0; i < 128; ++i) {
                REQUIRE(std::isfinite(out_l[static_cast<size_t>(i)]));
            }
        }
    }

    host.release();
}
