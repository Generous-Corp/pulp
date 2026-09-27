#pragma once
// Private diagnostics shared by the lifecycle and paired probes.
#include "detail/dawn_shared_io_provider.hpp"
#include "detail/shared_io_program_session.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif
using namespace pulp::gpu_audio::detail;
using Provider = DawnSharedIoProvider;
using Kind = SharedIoArenaProvider::StorageKind;
using Clock = std::chrono::steady_clock;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
std::uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

// The same overlap-save FFT graph, IR and oracle serve both storage modes.
constexpr unsigned n = 1024, frames = 128, channels = 2, taps = 257, slots = 3;
constexpr std::size_t words = 2 * n * channels;
std::vector<float> ir_spectrum() {
    std::vector<float> ir(2 * n);
    for (unsigned k = 0; k < n; ++k)
        for (unsigned t = 0; t < taps; ++t) {
            const double h = std::exp(-double(t) / 60.) * std::cos(double(t) * .31) / 80.;
            const double a = -2. * 3.14159265358979323846 * k * t / n;
            ir[2 * k] += float(h * std::cos(a) / n);
            ir[2 * k + 1] += float(h * std::sin(a) / n);
        }
    return ir;
}
std::unique_ptr<Provider> create(Provider::Fault fault = Provider::Fault::None) {
    auto result = Provider::create({.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
                                    .fault = fault,
                                    .completion_policy = Provider::CompletionPolicy::TimedWaitAny,
                                    .completion_wait_ns = 100000});
    require(bool(result.provider), result.reason.c_str());
    return std::move(result.provider);
}
void prepare(SharedIoProgramSession& session, std::unique_ptr<Provider>& owner, Kind kind) {
    require(owner->reconfigure_storage_kind(kind), "storage reconfigure refused");
    auto ir = ir_spectrum();
    auto program = owner->make_convolution_program({.fft_size = n,
                                                    .channels = channels,
                                                    .logical_frames = frames,
                                                    .ir_length = taps,
                                                    .normalized_ir_spectrum = ir});
    require(bool(program), "convolution program unavailable");
    require(session.prepare({std::move(owner), std::move(program)},
                            {.slots = slots,
                             .input_bytes_per_slot = words * sizeof(float),
                             .output_bytes_per_slot = words * sizeof(float),
                             .storage_kind = kind}),
            "session prepare failed");
}
std::unique_ptr<Provider> return_owner(SharedIoProgramSession& session) {
    auto base = session.release_to_owner();
    return std::unique_ptr<Provider>(static_cast<Provider*>(base.release()));
}
std::optional<SharedIoProgramSession::Completion> await(SharedIoProgramSession& session) {
    const auto limit = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < limit) {
        session.service_until(now_ns(), now_ns() + 100000);
        if (auto c = session.pop_completion())
            return c;
        std::this_thread::yield();
    }
    return {};
}
float sample(long absolute, unsigned ch) {
    return absolute < 0 ? 0.f
                        : float(.2 * std::sin(double(absolute) * (.019 + ch * .007)) +
                                ((absolute % 131) == 0 ? .13 * (ch + 1) : 0));
}
void fill(std::span<std::byte> bytes, unsigned seq) {
    auto* data = reinterpret_cast<float*>(bytes.data());
    for (unsigned ch = 0; ch < channels; ++ch)
        for (unsigned i = 0; i < n; ++i) {
            const long absolute = long(seq + 1) * frames - n + i;
            data[2 * (ch * n + i)] = sample(absolute, ch);
            data[2 * (ch * n + i) + 1] = 0;
        }
}
double check(std::span<const std::byte> bytes, unsigned seq) {
    const auto* data = reinterpret_cast<const float*>(bytes.data());
    double error = 0;
    for (unsigned ch = 0; ch < channels; ++ch)
        for (unsigned i = 0; i < frames; ++i) {
            double expected = 0;
            for (unsigned t = 0; t < taps; ++t)
                expected += sample(long(seq) * frames + i - t, ch) * std::exp(-double(t) / 60.) *
                            std::cos(double(t) * .31) / 80.;
            const double actual = data[2 * (ch * n + n - frames + i)];
            require(std::isfinite(actual), "nonfinite output");
            error = std::max(error, std::abs(actual - expected));
        }
    require(error < 1e-4, "FFT convolution oracle mismatch");
    return error;
}
