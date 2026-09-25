#include "dawn_shared_io_wavenet_program.hpp"
#include "dawn_shared_io_provider.hpp"

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

DawnSharedIoWavenetProgram::DawnSharedIoWavenetProgram(
    DawnSharedIoProvider* provider, const DawnSharedIoWavenetProgramSpec& spec)
    : provider_(provider), block_size_(spec.block_size), stream_instances_(spec.stream_instances),
      weights_(spec.weights.begin(), spec.weights.end()), head_scale_(spec.head_scale) {
    arrays_.reserve(spec.arrays.size());
    dilations_.reserve(spec.arrays.size());
    for (const auto& array : spec.arrays) {
        dilations_.emplace_back(array.dilations.begin(), array.dilations.end());
        auto copy = array;
        copy.dilations = dilations_.back();
        arrays_.push_back(copy);
    }
    if (const auto bytes = history_size(spec))
        history_.resize(*bytes);
}

std::unique_ptr<DawnSharedIoWavenetProgram>
DawnSharedIoWavenetProgram::create(DawnSharedIoProvider& provider,
                                   const DawnSharedIoWavenetProgramSpec& spec) {
    if (!validate_dawn_shared_io_wavenet_spec(spec).accepted())
        return nullptr;
    const auto bytes = history_size(spec);
    if (!bytes)
        return nullptr;
    try {
        (void)bytes;
        return std::unique_ptr<DawnSharedIoWavenetProgram>(
            new DawnSharedIoWavenetProgram(&provider, spec));
    } catch (...) {
        return nullptr;
    }
}

std::unique_ptr<DawnSharedIoWavenetProgram>
DawnSharedIoWavenetProgram::create(const DawnSharedIoWavenetProgramSpec& spec) {
    if (!validate_dawn_shared_io_wavenet_spec(spec).accepted())
        return nullptr;
    try {
        return std::unique_ptr<DawnSharedIoWavenetProgram>(
            new DawnSharedIoWavenetProgram(nullptr, spec));
    } catch (...) {
        return nullptr;
    }
}

bool DawnSharedIoWavenetProgram::prepare(SharedIoArenaProvider& provider,
                                         std::span<const SlotBufferHandle> slots) noexcept {
    if (prepared_ || slots.empty() || provider_ != &provider)
        return false;
    for (const auto& slot : slots)
        if (!provider.validate_slot_buffers(slot))
            return false;
    prepared_ = provider_->prepare_wavenet_program(
        {.block_size = block_size_,
         .head_scale = head_scale_,
         .stream_instances = stream_instances_,
         .arrays = arrays_,
         .weights = weights_},
        slots);
    return prepared_;
}

bool DawnSharedIoWavenetProgram::submit(SharedIoArenaProvider& provider,
                                        const SlotResources& resources, SlotToken token,
                                        std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept {
    return prepared_ && provider_ == &provider &&
           provider_->submit_wavenet_program(resources, token, std::move(terminal_inbox));
}

bool DawnSharedIoWavenetProgram::release() noexcept {
    if (prepared_ && provider_ != nullptr && !provider_->release_wavenet_program())
        return false;
    prepared_ = false;
    provider_ = nullptr;
    history_.clear();
    weights_.clear();
    return true;
}

} // namespace pulp::gpu_audio::detail
