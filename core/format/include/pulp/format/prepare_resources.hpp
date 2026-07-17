#pragma once

// Prepare-time resource caps and the PrepareContext a host passes to
// Processor::prepare(). processor.hpp includes this header, so existing
// consumers keep compiling unchanged; new code should include it directly.

#include <cstddef>

namespace pulp::format {

/// Prepare context — passed once before processing starts.
///
/// Contains the host's audio configuration. Use this to allocate
/// buffers, initialize filters at the correct sample rate, etc.
enum class PrepareResourceLimit {
    None = 0,
    PersistentBytes,
    BlockScratchBytes,
    TotalBytes,
    BlockSize,
    InputChannels,
    OutputChannels,
    ParameterEvents,
    MidiEvents,
    Voices,
};

/// Optional prepare-time resource caps supplied by a host or test harness.
///
/// A zero field means "unlimited/unspecified" so existing hosts preserve their
/// historical behavior. Processors that know their prepared working set can use
/// these caps to fail closed before allocating oversized scratch, sample caches,
/// event rings, or voice pools.
struct PrepareResourceLimits {
    std::size_t max_persistent_bytes = 0;
    std::size_t max_block_scratch_bytes = 0;
    std::size_t max_total_bytes = 0;
    int max_block_size = 0;
    int max_input_channels = 0;
    int max_output_channels = 0;
    int max_parameter_events = 0;
    int max_midi_events = 0;
    int max_voices = 0;
};

/// Processor-reported prepared resource usage estimate.
///
/// This is a preflight/accounting surface, not an allocator. Values should
/// represent storage that will be allocated or reserved during prepare(), plus
/// fixed per-block scratch required by process(). Hosts can compare this with
/// `PrepareResourceLimits` before committing a large prepare.
struct PrepareResourceUsage {
    std::size_t persistent_bytes = 0;
    std::size_t block_scratch_bytes = 0;
    int block_size = 0;
    int input_channels = 0;
    int output_channels = 0;
    int parameter_events = 0;
    int midi_events = 0;
    int voices = 0;

    std::size_t total_bytes() const noexcept {
        return persistent_bytes + block_scratch_bytes;
    }
};

inline PrepareResourceLimit first_exceeded_prepare_resource_limit(
    const PrepareResourceUsage& usage,
    const PrepareResourceLimits& limits) noexcept {
    auto over_size = [](std::size_t value, std::size_t limit) {
        return limit > 0 && value > limit;
    };
    auto over_int = [](int value, int limit) {
        return limit > 0 && value > limit;
    };

    if (over_size(usage.persistent_bytes, limits.max_persistent_bytes))
        return PrepareResourceLimit::PersistentBytes;
    if (over_size(usage.block_scratch_bytes, limits.max_block_scratch_bytes))
        return PrepareResourceLimit::BlockScratchBytes;
    if (over_size(usage.total_bytes(), limits.max_total_bytes))
        return PrepareResourceLimit::TotalBytes;
    if (over_int(usage.block_size, limits.max_block_size))
        return PrepareResourceLimit::BlockSize;
    if (over_int(usage.input_channels, limits.max_input_channels))
        return PrepareResourceLimit::InputChannels;
    if (over_int(usage.output_channels, limits.max_output_channels))
        return PrepareResourceLimit::OutputChannels;
    if (over_int(usage.parameter_events, limits.max_parameter_events))
        return PrepareResourceLimit::ParameterEvents;
    if (over_int(usage.midi_events, limits.max_midi_events))
        return PrepareResourceLimit::MidiEvents;
    if (over_int(usage.voices, limits.max_voices))
        return PrepareResourceLimit::Voices;
    return PrepareResourceLimit::None;
}

struct PrepareContext {
    double sample_rate = 48000.0;
    int max_buffer_size = 512;
    int input_channels = 2;
    int output_channels = 2;
    PrepareResourceLimits resource_limits;
};

} // namespace pulp::format
