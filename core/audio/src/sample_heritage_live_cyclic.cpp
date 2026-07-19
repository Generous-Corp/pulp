#include <pulp/audio/sample_heritage_live_cyclic.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace pulp::audio {
namespace {

bool checked_add(std::size_t a, std::size_t b, std::size_t& result) noexcept {
    if (b > std::numeric_limits<std::size_t>::max() - a)
        return false;
    result = a + b;
    return true;
}

bool checked_multiply(std::size_t a, std::size_t b, std::size_t& result) noexcept {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a)
        return false;
    result = a * b;
    return true;
}

bool next_power_of_two(std::size_t value, std::size_t& result) noexcept {
    if (value == 0)
        return false;
    --value;
    for (std::size_t shift = 1; shift < sizeof(value) * 8; shift <<= 1u)
        value |= value >> shift;
    if (value == std::numeric_limits<std::size_t>::max())
        return false;
    result = value + 1;
    return true;
}

std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    auto value = state;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31u);
}

} // namespace

SampleHeritageLiveCyclicResources SampleHeritageLiveCyclicStretch::resources_for(
    const SampleHeritageLiveCyclicConfig& config) noexcept {
    SampleHeritageLiveCyclicResources result;
    if (!std::isfinite(config.factor) ||
        config.factor < kSampleHeritageCyclicStretchMinimumFactor ||
        config.factor > kSampleHeritageCyclicStretchMaximumFactor || config.cycle_samples == 0 ||
        config.cycle_samples > kMaximumCycleSamples ||
        config.crossfade_samples > config.cycle_samples / 2 || config.shuffle_divisions == 0 ||
        config.shuffle_divisions > kMaximumDivisions ||
        config.shuffle_divisions > config.cycle_samples || config.max_block_samples == 0 ||
        config.channel_count == 0 || config.channel_count > kMaximumChannels) {
        return result;
    }

    const auto ratio = 1.0 / config.factor;
    const auto demand =
        std::ceil(static_cast<double>(config.max_block_samples) * std::max(1.0, ratio));
    if (!std::isfinite(demand) ||
        demand > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        result.status = SampleHeritageLiveCyclicStatus::SizeOverflow;
        return result;
    }

    std::size_t required = config.cycle_samples;
    if (!checked_add(required, config.crossfade_samples, required) ||
        !checked_add(required, static_cast<std::size_t>(demand), required) ||
        !checked_add(required, kInterpolatorGuardFrames, required) ||
        !next_power_of_two(required, result.ring_capacity_frames)) {
        result.status = SampleHeritageLiveCyclicStatus::SizeOverflow;
        return result;
    }

    std::size_t ring_samples = 0;
    std::size_t ring_bytes = 0;
    std::size_t permutation_count = 0;
    std::size_t permutation_bytes = 0;
    if (!checked_multiply(result.ring_capacity_frames, config.channel_count, ring_samples) ||
        !checked_multiply(ring_samples, sizeof(float), ring_bytes) ||
        !checked_multiply(config.channel_count, config.shuffle_divisions, permutation_count) ||
        !checked_multiply(permutation_count, sizeof(std::uint32_t), permutation_bytes) ||
        !checked_add(ring_bytes, permutation_bytes, result.persistent_bytes)) {
        result.status = SampleHeritageLiveCyclicStatus::SizeOverflow;
        result.ring_capacity_frames = 0;
        return result;
    }
    result.scratch_bytes = 0;
    result.status = SampleHeritageLiveCyclicStatus::Ok;
    return result;
}

SampleHeritageLiveCyclicStatus
SampleHeritageLiveCyclicStretch::prepare(const SampleHeritageLiveCyclicConfig& config) noexcept {
    release();
    const auto required = resources_for(config);
    if (!required.valid())
        return required.status;
    try {
        ring_.assign(required.ring_capacity_frames * config.channel_count, 0.0f);
        permutation_.assign(config.channel_count * config.shuffle_divisions, 0u);
    } catch (...) {
        release();
        return SampleHeritageLiveCyclicStatus::AllocationFailed;
    }
    config_ = config;
    resources_ = required;
    source_ratio_ = 1.0 / config.factor;
    exact_bypass_ = config.factor == 1.0;
    cold_lookahead_ =
        exact_bypass_ ? 0
                      : config.cycle_samples + config.crossfade_samples + kInterpolatorGuardFrames;
    prepared_ = true;
    reset();
    return SampleHeritageLiveCyclicStatus::Ok;
}

void SampleHeritageLiveCyclicStretch::release() noexcept {
    std::vector<float>().swap(ring_);
    std::vector<std::uint32_t>().swap(permutation_);
    config_ = {};
    resources_ = {};
    source_ratio_ = 1.0;
    cold_lookahead_ = 0;
    accepted_source_frames_ = 0;
    rendered_output_frames_ = 0;
    cycle_index_offset_ = 0;
    permutation_cycle_ = static_cast<std::uint64_t>(-1);
    prepared_ = false;
    exact_bypass_ = false;
}

void SampleHeritageLiveCyclicStretch::reset() noexcept {
    if (!prepared_)
        return;
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    accepted_source_frames_ = 0;
    rendered_output_frames_ = 0;
    cycle_index_offset_ = 0;
    permutation_cycle_ = static_cast<std::uint64_t>(-1);
}

void SampleHeritageLiveCyclicStretch::reset_with_rng_continuation(
    SampleHeritageLiveCyclicRngContinuation continuation) noexcept {
    reset();
    if (!prepared_)
        return;
    config_.seed = continuation.seed;
    cycle_index_offset_ = continuation.next_cycle_index;
}

std::uint64_t SampleHeritageLiveCyclicStretch::source_anchor(std::uint64_t cycle_index,
                                                             bool& overflow) const noexcept {
    const long double cycle_start =
        static_cast<long double>(cycle_index) * static_cast<long double>(config_.cycle_samples);
    const long double anchor =
        std::floor(cycle_start * static_cast<long double>(source_ratio_) + 0.5L);
    if (!std::isfinite(anchor) ||
        anchor > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        overflow = true;
        return 0;
    }
    return static_cast<std::uint64_t>(anchor);
}

std::uint64_t SampleHeritageLiveCyclicStretch::required_source_through(
    std::uint64_t first_output, std::uint64_t output_frames, bool& overflow) const noexcept {
    if (output_frames == 0)
        return accepted_source_frames_;
    if (output_frames - 1 > std::numeric_limits<std::uint64_t>::max() - first_output) {
        overflow = true;
        return 0;
    }

    const auto last_output = first_output + output_frames - 1;
    const auto first_cycle = first_output / config_.cycle_samples;
    const auto last_cycle = last_output / config_.cycle_samples;
    const auto last_anchor = source_anchor(last_cycle, overflow);
    if (overflow || last_anchor > std::numeric_limits<std::uint64_t>::max() -
                                      config_.cycle_samples - kInterpolatorGuardFrames) {
        overflow = true;
        return 0;
    }
    auto required = last_anchor + config_.cycle_samples + kInterpolatorGuardFrames;

    const auto first_phase = first_output % config_.cycle_samples;
    const bool touches_cycle_crossfade =
        last_cycle > 0 && (first_cycle < last_cycle || first_phase < config_.crossfade_samples);
    if (touches_cycle_crossfade) {
        const auto previous_anchor = source_anchor(last_cycle - 1, overflow);
        if (overflow || previous_anchor > std::numeric_limits<std::uint64_t>::max() -
                                              config_.cycle_samples - config_.crossfade_samples -
                                              kInterpolatorGuardFrames) {
            overflow = true;
            return 0;
        }
        required = std::max(required, previous_anchor + config_.cycle_samples +
                                          config_.crossfade_samples + kInterpolatorGuardFrames);
    }
    return std::max(required, static_cast<std::uint64_t>(cold_lookahead_));
}

SampleHeritageLiveCyclicPlan
SampleHeritageLiveCyclicStretch::plan(std::size_t output_frames) const noexcept {
    SampleHeritageLiveCyclicPlan result;
    result.output_frames = output_frames;
    result.cold_lookahead_frames = cold_lookahead_;
    if (!prepared_)
        return result;
    if (output_frames > config_.max_block_samples ||
        output_frames > std::numeric_limits<std::uint64_t>::max() - rendered_output_frames_) {
        result.status = SampleHeritageLiveCyclicStatus::InvalidDimensions;
        return result;
    }
    if (exact_bypass_) {
        result.status = SampleHeritageLiveCyclicStatus::Ok;
        result.input_frames = output_frames;
        return result;
    }
    bool overflow = false;
    const auto target = required_source_through(
        rendered_output_frames_, static_cast<std::uint64_t>(output_frames), overflow);
    if (overflow) {
        result.status = SampleHeritageLiveCyclicStatus::SizeOverflow;
        return result;
    }
    const auto needed = target > accepted_source_frames_ ? target - accepted_source_frames_ : 0;
    if (needed > std::numeric_limits<std::size_t>::max()) {
        result.status = SampleHeritageLiveCyclicStatus::SizeOverflow;
        return result;
    }
    result.status = SampleHeritageLiveCyclicStatus::Ok;
    result.input_frames = static_cast<std::size_t>(needed);
    return result;
}

bool SampleHeritageLiveCyclicStretch::fill_permutation(std::uint64_t cycle_index,
                                                       std::size_t channel,
                                                       std::uint32_t* destination) const noexcept {
    if (!prepared_ || channel >= config_.channel_count || destination == nullptr)
        return false;
    for (std::size_t division = 0; division < config_.shuffle_divisions; ++division)
        destination[division] = static_cast<std::uint32_t>(division);
    if (config_.shuffle == SampleHeritageLiveCyclicShuffle::Identity ||
        config_.shuffle_divisions == 1)
        return true;

    const auto stream = config_.linked_channels ? 0u : static_cast<std::uint64_t>(channel);
    auto state =
        config_.seed ^ (cycle_index * 0xd1342543de82ef95ULL) ^ (stream * 0xa24baed4963ee407ULL);
    for (std::size_t count = config_.shuffle_divisions; count > 1; --count) {
        const auto selected = static_cast<std::size_t>(splitmix64(state) % count);
        std::swap(destination[count - 1], destination[selected]);
    }
    return true;
}

bool SampleHeritageLiveCyclicStretch::division_permutation(
    std::uint64_t cycle_index, std::size_t channel,
    std::span<std::uint32_t> destination) const noexcept {
    return destination.size() == config_.shuffle_divisions &&
           fill_permutation(cycle_index + cycle_index_offset_, channel, destination.data());
}

std::uint32_t* SampleHeritageLiveCyclicStretch::permutation_slot(std::size_t channel) noexcept {
    return permutation_.data() + channel * config_.shuffle_divisions;
}

std::size_t SampleHeritageLiveCyclicStretch::mapped_cycle_offset(
    std::size_t phase, std::uint64_t cycle_index, std::size_t channel, bool& join_crossfade,
    std::size_t& alternate_offset, float& join_weight) noexcept {
    if (permutation_cycle_ != cycle_index) {
        for (std::size_t ch = 0; ch < config_.channel_count; ++ch)
            fill_permutation(cycle_index + cycle_index_offset_, ch, permutation_slot(ch));
        permutation_cycle_ = cycle_index;
    }
    const auto* permutation = permutation_slot(channel);
    const auto divisions = config_.shuffle_divisions;
    const auto division =
        std::min(divisions - 1, ((phase + 1) * divisions - 1) / config_.cycle_samples);
    const auto output_begin = division * config_.cycle_samples / divisions;
    const auto output_end = (division + 1) * config_.cycle_samples / divisions;
    const auto source_division = static_cast<std::size_t>(permutation[division]);
    const auto source_begin = source_division * config_.cycle_samples / divisions;
    const auto local = phase - output_begin;
    const auto mapped = source_begin + local;

    join_crossfade = false;
    alternate_offset = mapped;
    join_weight = 1.0f;
    if (config_.shuffle == SampleHeritageLiveCyclicShuffle::FisherYates && division > 0 &&
        config_.crossfade_samples > 0) {
        const auto width = std::min({config_.crossfade_samples, output_end - output_begin,
                                     config_.cycle_samples / divisions});
        if (width > 0 && local < width) {
            const auto previous_source = static_cast<std::size_t>(permutation[division - 1]);
            const auto previous_begin = previous_source * config_.cycle_samples / divisions;
            const auto previous_end = (previous_source + 1) * config_.cycle_samples / divisions;
            const auto previous_width = previous_end - previous_begin;
            alternate_offset = previous_end - std::min(width, previous_width) + local;
            alternate_offset = std::min(alternate_offset, previous_end - 1);
            join_weight =
                width == 1 ? 1.0f : static_cast<float>(local) / static_cast<float>(width - 1);
            join_crossfade = true;
        }
    }
    return mapped;
}

float SampleHeritageLiveCyclicStretch::read_linear(std::size_t channel, double position,
                                                   bool& ok) const noexcept {
    if (!std::isfinite(position) || position < 0.0) {
        ok = false;
        return 0.0f;
    }
    const auto first = static_cast<std::uint64_t>(std::floor(position));
    if (first >= accepted_source_frames_ || first + 1 >= accepted_source_frames_ ||
        accepted_source_frames_ - first > resources_.ring_capacity_frames) {
        ok = false;
        return 0.0f;
    }
    const auto mask = resources_.ring_capacity_frames - 1;
    const auto base = channel * resources_.ring_capacity_frames;
    const auto fraction = static_cast<float>(position - static_cast<double>(first));
    const auto a = ring_[base + (static_cast<std::size_t>(first) & mask)];
    const auto b = ring_[base + (static_cast<std::size_t>(first + 1) & mask)];
    return a + (b - a) * fraction;
}

void SampleHeritageLiveCyclicStretch::write_input(BufferView<const float> input) noexcept {
    const auto mask = resources_.ring_capacity_frames - 1;
    for (std::size_t frame = 0; frame < input.num_samples(); ++frame) {
        const auto slot = static_cast<std::size_t>(accepted_source_frames_ + frame) & mask;
        for (std::size_t channel = 0; channel < config_.channel_count; ++channel)
            ring_[channel * resources_.ring_capacity_frames + slot] = input.channel(channel)[frame];
    }
    accepted_source_frames_ += input.num_samples();
}

SampleHeritageLiveCyclicStatus
SampleHeritageLiveCyclicStretch::process(BufferView<const float> input,
                                         BufferView<float> output) noexcept {
    const auto expected = plan(output.num_samples());
    if (!expected.valid())
        return expected.status;
    if (input.num_channels() != config_.channel_count ||
        output.num_channels() != config_.channel_count)
        return SampleHeritageLiveCyclicStatus::InvalidDimensions;
    if (input.num_samples() != expected.input_frames)
        return SampleHeritageLiveCyclicStatus::InputFrameMismatch;

    if (exact_bypass_) {
        for (std::size_t channel = 0; channel < config_.channel_count; ++channel)
            std::copy(input.channel(channel).begin(), input.channel(channel).end(),
                      output.channel(channel).begin());
        accepted_source_frames_ += input.num_samples();
        rendered_output_frames_ += output.num_samples();
        return SampleHeritageLiveCyclicStatus::Ok;
    }

    write_input(input);
    bool available = true;
    for (std::size_t frame = 0; frame < output.num_samples(); ++frame) {
        const auto absolute_output = rendered_output_frames_ + frame;
        const auto cycle = absolute_output / config_.cycle_samples;
        const auto phase = static_cast<std::size_t>(absolute_output % config_.cycle_samples);
        bool anchor_overflow = false;
        const auto cycle_anchor = source_anchor(cycle, anchor_overflow);
        if (anchor_overflow) {
            output.clear();
            return SampleHeritageLiveCyclicStatus::SizeOverflow;
        }
        for (std::size_t channel = 0; channel < config_.channel_count; ++channel) {
            const auto permutation_channel = config_.linked_channels ? 0 : channel;
            bool join = false;
            std::size_t alternate = 0;
            float join_weight = 1.0f;
            const auto mapped = mapped_cycle_offset(phase, cycle, permutation_channel, join,
                                                    alternate, join_weight);
            auto value =
                read_linear(channel, static_cast<double>(cycle_anchor + mapped), available);
            if (cycle > 0 && phase < config_.crossfade_samples) {
                const auto previous_anchor = source_anchor(cycle - 1, anchor_overflow);
                if (anchor_overflow) {
                    output.clear();
                    return SampleHeritageLiveCyclicStatus::SizeOverflow;
                }
                const auto old_value = read_linear(
                    channel, static_cast<double>(previous_anchor + config_.cycle_samples + phase),
                    available);
                const auto weight = config_.crossfade_samples == 1
                                        ? 1.0f
                                        : static_cast<float>(phase) /
                                              static_cast<float>(config_.crossfade_samples - 1);
                value = old_value + (value - old_value) * weight;
            } else if (join) {
                const auto old_value =
                    read_linear(channel, static_cast<double>(cycle_anchor + alternate), available);
                value = old_value + (value - old_value) * join_weight;
            }
            output.channel(channel)[frame] = available ? value : 0.0f;
        }
    }
    rendered_output_frames_ += output.num_samples();
    return available ? SampleHeritageLiveCyclicStatus::Ok
                     : SampleHeritageLiveCyclicStatus::SourceUnavailable;
}

SampleHeritageLiveCyclicRngContinuation
SampleHeritageLiveCyclicStretch::capture_next_cycle_rng_continuation() const noexcept {
    const auto next_cycle =
        config_.cycle_samples == 0
            ? 0
            : (rendered_output_frames_ + config_.cycle_samples - 1) / config_.cycle_samples;
    return {config_.seed, cycle_index_offset_ + next_cycle};
}

static_assert(!std::is_copy_constructible_v<SampleHeritageLiveCyclicStretch>);
static_assert(!std::is_move_constructible_v<SampleHeritageLiveCyclicStretch>);

} // namespace pulp::audio
