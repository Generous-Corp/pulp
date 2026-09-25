#include "dawn_shared_io_wavenet_program.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace pulp::gpu_audio::detail {

namespace {
std::optional<std::size_t> history_size(const DawnSharedIoWavenetProgramSpec& spec) noexcept {
    std::uint64_t samples = 0;
    for (const auto& layer : spec.arrays) {
        std::uint32_t max_history = 0;
        for (const auto dilation : layer.dilations) {
            const auto span = static_cast<std::uint64_t>(layer.kernel - 1u) * dilation + 1u;
            if (span > std::numeric_limits<std::uint32_t>::max())
                return std::nullopt;
            max_history = std::max(max_history, static_cast<std::uint32_t>(span));
        }
        samples += static_cast<std::uint64_t>(max_history) * layer.channels;
    }
    samples *= spec.stream_instances;
    if (samples > std::numeric_limits<std::size_t>::max() / sizeof(float))
        return std::nullopt;
    return static_cast<std::size_t>(samples) * sizeof(float);
}
} // namespace

DawnSharedIoWavenetProgram::DawnSharedIoWavenetProgram(std::uint32_t block_size,
                                                       std::uint32_t stream_instances,
                                                       std::vector<float> weights,
                                                       std::size_t history_bytes)
    : block_size_(block_size), stream_instances_(stream_instances), weights_(std::move(weights)),
      history_(history_bytes) {}

std::unique_ptr<DawnSharedIoWavenetProgram>
DawnSharedIoWavenetProgram::create(const DawnSharedIoWavenetProgramSpec& spec) {
    if (!validate_dawn_shared_io_wavenet_spec(spec).accepted())
        return nullptr;
    const auto bytes = history_size(spec);
    if (!bytes)
        return nullptr;
    try {
        std::vector<float> weights(spec.weights.begin(), spec.weights.end());
        return std::unique_ptr<DawnSharedIoWavenetProgram>(new DawnSharedIoWavenetProgram(
            spec.block_size, spec.stream_instances, std::move(weights), *bytes));
    } catch (...) {
        return nullptr;
    }
}

bool DawnSharedIoWavenetProgram::prepare(SharedIoArenaProvider& provider,
                                         std::span<const SlotBufferHandle> slots) noexcept {
    if (prepared_ || slots.empty())
        return false;
    for (const auto& slot : slots)
        if (!provider.validate_slot_buffers(slot))
            return false;
    prepared_ = true;
    return true;
}

bool DawnSharedIoWavenetProgram::submit(SharedIoArenaProvider&, const SlotResources&, SlotToken,
                                        std::shared_ptr<SharedIoTerminalInbox>) noexcept {
    // No Dawn pipeline/device ownership exists in this compile-gated slice.
    // Returning false is the permitted pre-submit refusal and keeps callers on
    // their explicit CPU fallback path.
    return false;
}

bool DawnSharedIoWavenetProgram::release() noexcept {
    prepared_ = false;
    history_.clear();
    weights_.clear();
    return true;
}

} // namespace pulp::gpu_audio::detail
