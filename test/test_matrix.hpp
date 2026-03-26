#pragma once

/// @file test_matrix.hpp
/// Reusable test matrix for sweeping sample rates, buffer sizes, and
/// edge cases across all Pulp plugins via HeadlessHost.

#include <pulp/format/headless.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/midi/buffer.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>
#include <string>
#include <functional>

namespace pulp::test {

/// Standard sample rates to test.
inline constexpr double kSampleRates[] = {22050, 44100, 48000, 88200, 96000, 192000};

/// Standard buffer sizes to test.
inline constexpr int kBufferSizes[] = {1, 32, 64, 128, 256, 512, 1024, 2048, 4096};

/// Run a processor through all sample rate / buffer size combinations.
/// The callback receives (host, sample_rate, buffer_size) and should
/// return true if the test passed.
inline void sweep_sample_rates_and_buffers(
    format::ProcessorFactory factory,
    std::function<bool(format::HeadlessHost&, double, int)> test_fn)
{
    for (double sr : kSampleRates) {
        for (int bs : kBufferSizes) {
            format::HeadlessHost host(factory);
            host.prepare(sr, bs);

            INFO("sample_rate=" << sr << " buffer_size=" << bs);
            REQUIRE(test_fn(host, sr, bs));

            host.release();
        }
    }
}

/// Verify a plugin produces finite output (no NaN/Inf) with silence input.
inline void verify_silence_produces_finite(format::ProcessorFactory factory) {
    for (double sr : {44100.0, 96000.0}) {
        for (int bs : {64, 512}) {
            format::HeadlessHost host(factory);
            host.prepare(sr, bs);

            std::vector<float> in_l(static_cast<size_t>(bs), 0.0f);
            std::vector<float> in_r(static_cast<size_t>(bs), 0.0f);
            std::vector<float> out_l(static_cast<size_t>(bs));
            std::vector<float> out_r(static_cast<size_t>(bs));

            const float* in_ptrs[] = {in_l.data(), in_r.data()};
            float* out_ptrs[] = {out_l.data(), out_r.data()};
            audio::BufferView<const float> in_view(in_ptrs, 2, bs);
            audio::BufferView<float> out_view(out_ptrs, 2, bs);

            host.process(out_view, in_view);

            INFO("silence finite check: sr=" << sr << " bs=" << bs);
            for (int i = 0; i < bs; ++i) {
                REQUIRE(std::isfinite(out_l[static_cast<size_t>(i)]));
                REQUIRE(std::isfinite(out_r[static_cast<size_t>(i)]));
            }

            host.release();
        }
    }
}

/// Verify a plugin produces finite output with impulse input.
inline void verify_impulse_produces_finite(format::ProcessorFactory factory) {
    format::HeadlessHost host(factory);
    host.prepare(44100, 512);

    std::vector<float> in_l(512, 0.0f);
    std::vector<float> in_r(512, 0.0f);
    in_l[0] = 1.0f; // impulse
    in_r[0] = 1.0f;

    std::vector<float> out_l(512);
    std::vector<float> out_r(512);

    const float* in_ptrs[] = {in_l.data(), in_r.data()};
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<const float> in_view(in_ptrs, 2, 512);
    audio::BufferView<float> out_view(out_ptrs, 2, 512);

    host.process(out_view, in_view);

    for (int i = 0; i < 512; ++i) {
        REQUIRE(std::isfinite(out_l[static_cast<size_t>(i)]));
        REQUIRE(std::isfinite(out_r[static_cast<size_t>(i)]));
    }

    host.release();
}

/// Verify a plugin handles DC offset without blowing up.
inline void verify_dc_offset_stable(format::ProcessorFactory factory) {
    format::HeadlessHost host(factory);
    host.prepare(44100, 512);

    std::vector<float> in_l(512, 0.5f); // DC offset
    std::vector<float> in_r(512, 0.5f);
    std::vector<float> out_l(512);
    std::vector<float> out_r(512);

    const float* in_ptrs[] = {in_l.data(), in_r.data()};
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<const float> in_view(in_ptrs, 2, 512);
    audio::BufferView<float> out_view(out_ptrs, 2, 512);

    // Process multiple blocks to check for instability
    for (int block = 0; block < 10; ++block) {
        host.process(out_view, in_view);

        for (int i = 0; i < 512; ++i) {
            REQUIRE(std::isfinite(out_l[static_cast<size_t>(i)]));
            REQUIRE(std::abs(out_l[static_cast<size_t>(i)]) < 100.0f); // reasonable bound
        }
    }

    host.release();
}

/// Verify state round-trip works at the plugin level.
inline void verify_state_round_trip(format::ProcessorFactory factory) {
    format::HeadlessHost host(factory);
    host.prepare(44100, 512);

    // Set all params to non-default values
    auto params = host.state().all_params();
    for (const auto& p : params) {
        float mid = (p.range.min + p.range.max) / 2.0f;
        host.state().set_value(p.id, mid);
    }

    // Save state
    auto saved = host.save_state();
    REQUIRE_FALSE(saved.empty());

    // Reset to defaults
    host.state().reset_all_to_defaults();

    // Restore state
    REQUIRE(host.load_state(saved));

    // Verify values restored
    for (const auto& p : params) {
        float mid = (p.range.min + p.range.max) / 2.0f;
        float restored = host.state().get_value(p.id);
        REQUIRE(std::abs(restored - mid) < 0.01f);
    }

    host.release();
}

/// Verify a plugin survives zero-length buffer.
inline void verify_zero_length_buffer(format::ProcessorFactory factory) {
    format::HeadlessHost host(factory);
    host.prepare(44100, 512);

    const float* in_ptrs[] = {nullptr, nullptr};
    float* out_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in_view(in_ptrs, 2, 0);
    audio::BufferView<float> out_view(out_ptrs, 2, 0);

    // Should not crash
    host.process(out_view, in_view);
    host.release();
}

} // namespace pulp::test
