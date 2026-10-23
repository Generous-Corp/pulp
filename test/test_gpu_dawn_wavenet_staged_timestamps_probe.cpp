// Physical proof that the private WaveNet provider keeps completing blocks with
// staged storage and opt-in GPU timestamps, and that this diagnostic mode does not
// change the audio. For a standard (ungated) and a gated two-array net, six
// blocks are streamed through two slots twice: once with imported storage and
// once with staged storage plus timestamps. Every block must reach a terminal
// within a bounded deadline, and the two streams must match byte for byte.
// A block that never completes is reported as a failure, not a hang.

#include "detail/dawn_shared_io_provider.hpp"
#include "detail/dawn_shared_io_wavenet_program.hpp"
#include "detail/shared_io_compute_plan.hpp"
#include "detail/shared_io_wavenet_spec.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif

namespace {

using namespace pulp::gpu_audio::detail;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kBlock = 64;
constexpr std::uint64_t kBlocks = 6;
constexpr auto kCompletionDeadline = std::chrono::seconds(3);

struct Net {
    std::string name;
    std::vector<std::uint32_t> d0, d1;
    std::vector<WavenetLayerSpec> arrays;
    std::vector<float> weights;

    // The layer specs point into d0/d1, so a Net is never copied or moved.
    Net(const Net&) = delete;
    Net& operator=(const Net&) = delete;

    Net(std::string label, bool gated) : name(std::move(label)) {
        const std::uint32_t g = gated ? 1u : 0u;
        if (gated) {
            d0 = {1, 2, 4, 8, 16, 32};
            d1 = {64, 1, 2, 4};
            arrays = {{1, 1, 8, 3, 4, g, 0, d0}, {8, 1, 4, 3, 1, g, 1, d1}};
        } else {
            for (std::uint32_t d = 1; d <= 512; d *= 2) {
                d0.push_back(d);
                d1.push_back(d);
            }
            arrays = {{1, 1, 16, 3, 8, g, 0, d0}, {16, 1, 8, 3, 1, g, 1, d1}};
        }
        std::uint64_t need = 0;
        for (const auto& a : arrays) {
            const std::uint64_t C = a.channels, Z = a.gated ? 2u * C : C;
            need += C * a.input_size;
            need += a.dilations.size() * (Z * C * a.kernel + Z + Z + C * C + C);
            need += a.head_size * C + (a.head_bias ? a.head_size : 0u);
        }
        weights.resize(need + 1u);
        std::uint32_t r = 0x2468ace1u;
        for (auto& w : weights) {
            r = r * 1664525u + 1013904223u;
            w = (static_cast<float>(r >> 8) / 16777216.0f - 0.5f) * 0.3f;
        }
        weights.back() = 0.75f;
    }

    WavenetProgramSpec spec() const noexcept {
        return {.block_size = kBlock,
                .head_scale = weights.back(),
                .stream_instances = 1,
                .arrays = arrays,
                .weights = weights};
    }
};

enum class StreamResult { Ok, Unavailable, Failed };

// Streams kBlocks blocks and appends every output sample to `out`. `stage`
// names the step that failed.
StreamResult stream(const Net& net, bool staged_with_timestamps, std::vector<float>& out,
                    std::string& stage) {
    const auto kind = staged_with_timestamps
                          ? SharedIoArenaProvider::StorageKind::Staged
                          : SharedIoArenaProvider::StorageKind::ImportedHostPointer;
    auto created =
        DawnSharedIoProvider::create({.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
                                      .storage_kind = kind,
                                      .enable_timestamps = staged_with_timestamps});
    if (!created.provider)
        return StreamResult::Unavailable;
    auto program = DawnSharedIoWavenetProgram::create(*created.provider, net.spec());
    if (!program) {
        stage = "program_create";
        return StreamResult::Failed;
    }
    SharedIoComputePlan plan;
    constexpr std::size_t bytes = kBlock * sizeof(float);
    if (!plan.prepare(*created.provider,
                      {.slots = 2,
                       .input_bytes_per_slot = bytes,
                       .output_bytes_per_slot = bytes,
                       .storage_kind = kind},
                      std::move(program))) {
        stage = "plan_prepare";
        return StreamResult::Failed;
    }
    std::vector<float> input(kBlock);
    for (std::uint64_t sequence = 1; sequence <= kBlocks; ++sequence) {
        for (std::uint32_t t = 0; t < kBlock; ++t) {
            const double n = static_cast<double>((sequence - 1u) * kBlock + t);
            input[t] = static_cast<float>(0.6 * std::sin(0.013 * n) * std::cos(0.0021 * n) +
                                          0.15 * std::sin(1.3 * n));
        }
        auto write = plan.acquire_input(sequence, 0);
        if (!write || write->bytes.size() != bytes) {
            stage = "input_lease_block_" + std::to_string(sequence);
            return StreamResult::Failed;
        }
        std::memcpy(write->bytes.data(), input.data(), bytes);
        if (!plan.submit({write->token, 0})) {
            stage = "submit_block_" + std::to_string(sequence);
            return StreamResult::Failed;
        }
        std::optional<SharedIoComputePlan::Completion> completion;
        const auto deadline = Clock::now() + kCompletionDeadline;
        while (!completion && Clock::now() < deadline) {
            plan.drain(std::numeric_limits<std::uint64_t>::max());
            completion = plan.pop_completion();
            if (!completion)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        if (!completion) {
            stage = "completion_timeout_block_" + std::to_string(sequence);
            (void)plan.release();
            return StreamResult::Failed;
        }
        auto output = plan.acquire_output(*completion);
        if (!output || output->bytes.size() != bytes) {
            stage = "output_block_" + std::to_string(sequence);
            return StreamResult::Failed;
        }
        const auto* samples = reinterpret_cast<const float*>(output->bytes.data());
        out.insert(out.end(), samples, samples + kBlock);
        if (!plan.release_output({output->token})) {
            stage = "output_release_block_" + std::to_string(sequence);
            return StreamResult::Failed;
        }
    }
    if (!plan.release() || !created.provider->drain()) {
        stage = "release";
        return StreamResult::Failed;
    }
    return StreamResult::Ok;
}

} // namespace

int main() {
    const Net standard("standard", false), gated("gated", true);
    for (const Net* net_ptr : {&standard, &gated}) {
        const Net& net = *net_ptr;
        if (!validate_wavenet_spec(net.spec()).accepted()) {
            std::cerr << "staged_timestamps_proof net=" << net.name
                      << " status=failed stage=fixture_validation\n";
            return 1;
        }
        std::vector<float> imported, staged;
        std::string stage;
        const auto reference = stream(net, false, imported, stage);
        if (reference == StreamResult::Unavailable)
            return 77;
        if (reference == StreamResult::Failed) {
            std::cerr << "staged_timestamps_proof net=" << net.name
                      << " mode=imported status=failed stage=" << stage << '\n';
            return 1;
        }
        const auto timed = stream(net, true, staged, stage);
        if (timed == StreamResult::Unavailable)
            return 77;
        if (timed == StreamResult::Failed) {
            std::cerr << "staged_timestamps_proof net=" << net.name
                      << " mode=staged_timestamps status=failed stage=" << stage << '\n';
            return 1;
        }
        if (imported.size() != staged.size() ||
            std::memcmp(imported.data(), staged.data(), imported.size() * sizeof(float)) != 0) {
            std::size_t first = 0;
            while (first < imported.size() && first < staged.size() &&
                   std::memcmp(&imported[first], &staged[first], sizeof(float)) == 0)
                ++first;
            std::cerr << "staged_timestamps_proof net=" << net.name
                      << " status=failed stage=byte_mismatch first_sample=" << first << '\n';
            return 1;
        }
        std::cout << "staged_timestamps_proof net=" << net.name << " blocks=" << kBlocks
                  << " slots=2 status=passed\n";
    }
    return 0;
}
