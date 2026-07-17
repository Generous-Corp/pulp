#include <pulp/format/transport_quantizer.hpp>
#include <pulp/timebase/quantize.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::format {

TransportQuantizer::BlockPlan::BlockPlan(const TransportQuantizer& owner,
                                         const ProcessContext& context,
                                         TransportQuantizerBlock block) noexcept
    : owner_(owner), context_(context), block_(block) {}

TransportQuantizeResult TransportQuantizer::BlockPlan::resolve(
    const TransportQuantizeRequest& request) const noexcept {
    return owner_.resolve(context_, request, block_);
}

void TransportQuantizer::reset() noexcept {
    last_ = {};
}

TransportQuantizerBlock TransportQuantizer::begin_block(
    const ProcessContext& context) noexcept {
    TransportQuantizerBlock block;
    block.timeline_valid = valid_timeline(context);
    block.transport_jumped = detect_transport_jump(context, block.timeline_valid);
    remember(context, block.timeline_valid);
    return block;
}

TransportQuantizer::BlockPlan TransportQuantizer::begin_block_plan(
    const ProcessContext& context) noexcept {
    return BlockPlan(*this, context, begin_block(context));
}

TransportQuantizeResult TransportQuantizer::resolve(
    const ProcessContext& context,
    const TransportQuantizeRequest& request,
    TransportQuantizerBlock block) const noexcept {
    TransportQuantizeResult result;
    result.transport_jumped = block.transport_jumped;

    if (block.transport_jumped && request.cancel_on_transport_jump) {
        result.status = TransportQuantizeStatus::TransportJumped;
        return result;
    }

    if (request.require_playing && !context.is_playing) {
        result.status = TransportQuantizeStatus::TransportStopped;
        return result;
    }

    if (context.num_samples < 0) {
        result.status = TransportQuantizeStatus::InvalidBlockSize;
        return result;
    }

    if (request.policy == TransportQuantizePolicy::Immediate) {
        result.scheduled = true;
        result.block_offset = 0;
        result.target_beats = context.position_beats;
        result.status = TransportQuantizeStatus::Scheduled;
        return result;
    }

    if (!valid_sample_rate(context.sample_rate)) {
        result.status = TransportQuantizeStatus::InvalidSampleRate;
        return result;
    }
    if (!valid_tempo(context.tempo_bpm)) {
        result.status = TransportQuantizeStatus::InvalidTempo;
        return result;
    }
    if (!block.timeline_valid) {
        result.status = TransportQuantizeStatus::InvalidTimeline;
        return result;
    }

    double target_beats = context.position_beats;
    switch (request.policy) {
        case TransportQuantizePolicy::Immediate:
            break;
        case TransportQuantizePolicy::NextBeat:
            target_beats = next_grid_boundary(context.position_beats, 1.0);
            break;
        case TransportQuantizePolicy::NextBar: {
            const auto bar_beats = beats_per_bar(context.time_sig_numerator,
                                                 context.time_sig_denominator);
            if (!(bar_beats > 0.0)) {
                result.status = TransportQuantizeStatus::InvalidTimeSignature;
                return result;
            }
            target_beats = next_grid_boundary(context.position_beats, bar_beats);
            break;
        }
        case TransportQuantizePolicy::NextGrid:
            if (!valid_grid(request.grid_beats)) {
                result.status = TransportQuantizeStatus::InvalidGrid;
                return result;
            }
            target_beats = next_grid_boundary(context.position_beats,
                                              request.grid_beats);
            break;
        case TransportQuantizePolicy::HostLoopStart:
            if (!valid_loop(context)) {
                result.status = TransportQuantizeStatus::LoopUnavailable;
                return result;
            }
            target_beats = next_host_loop_start_boundary(context);
            if (!std::isfinite(target_beats)) {
                result.status = TransportQuantizeStatus::LoopUnavailable;
                return result;
            }
            break;
    }

    return offset_for_target(context, target_beats, block.transport_jumped);
}

double TransportQuantizer::beats_per_bar(int numerator, int denominator) noexcept {
    return timebase::beats_per_bar(numerator, denominator);
}

bool TransportQuantizer::valid_sample_rate(double sample_rate) noexcept {
    return timebase::valid_sample_rate(sample_rate);
}

bool TransportQuantizer::valid_tempo(double tempo_bpm) noexcept {
    return timebase::valid_tempo(tempo_bpm);
}

bool TransportQuantizer::valid_grid(double grid_beats) noexcept {
    return timebase::valid_grid(grid_beats);
}

bool TransportQuantizer::valid_timeline(const ProcessContext& context) noexcept {
    return context.num_samples >= 0 && valid_sample_rate(context.sample_rate) &&
           valid_tempo(context.tempo_bpm) && std::isfinite(context.position_beats);
}

bool TransportQuantizer::valid_loop_range(double start_beats,
                                          double end_beats) noexcept {
    return std::isfinite(start_beats) && std::isfinite(end_beats) &&
           end_beats > start_beats;
}

bool TransportQuantizer::valid_loop(const ProcessContext& context) noexcept {
    return context.is_looping &&
           valid_loop_range(context.loop_start_beats, context.loop_end_beats);
}

double TransportQuantizer::frames_to_beats(double frames,
                                           double sample_rate,
                                           double tempo_bpm) noexcept {
    return timebase::frames_to_beats(frames, sample_rate, tempo_bpm);
}

double TransportQuantizer::beats_to_frames(double beats,
                                           double sample_rate,
                                           double tempo_bpm) noexcept {
    return timebase::beats_to_frames(beats, sample_rate, tempo_bpm);
}

double TransportQuantizer::next_grid_boundary(double position_beats,
                                              double grid_beats) noexcept {
    return timebase::next_grid_boundary(position_beats, grid_beats);
}

double TransportQuantizer::next_host_loop_start_boundary(
    const ProcessContext& context) noexcept {
    if (context.position_beats <=
        context.loop_start_beats + timebase::kBoundaryEpsilonBeats) {
        return context.loop_start_beats;
    }
    if (context.position_beats <
        context.loop_end_beats - timebase::kBoundaryEpsilonBeats) {
        return context.loop_end_beats;
    }
    return std::nan("");
}

double TransportQuantizer::wrap_loop_position(double position_beats,
                                              double loop_start_beats,
                                              double loop_end_beats) noexcept {
    const auto loop_length = loop_end_beats - loop_start_beats;
    if (!(loop_length > 0.0)) return position_beats;

    auto relative = std::fmod(position_beats - loop_start_beats, loop_length);
    if (relative < 0.0) relative += loop_length;
    return loop_start_beats + relative;
}

double TransportQuantizer::timeline_tolerance_beats(
    const ProcessContext& context) noexcept {
    if (!valid_timeline(context)) return timebase::kBoundaryEpsilonBeats;
    return std::max(timebase::kBoundaryEpsilonBeats,
                    frames_to_beats(2.0, context.sample_rate, context.tempo_bpm));
}

TransportQuantizeResult TransportQuantizer::offset_for_target(
    const ProcessContext& context,
    double target_beats,
    bool transport_jumped) const noexcept {
    TransportQuantizeResult result;
    result.transport_jumped = transport_jumped;
    result.target_beats = target_beats;

    const auto delta_beats = target_beats - context.position_beats;
    if (delta_beats < -timebase::kBoundaryEpsilonBeats) {
        result.status = TransportQuantizeStatus::OutsideBlock;
        return result;
    }

    const auto frames = beats_to_frames(std::max(0.0, delta_beats),
                                        context.sample_rate,
                                        context.tempo_bpm);
    if (!std::isfinite(frames)) {
        result.status = TransportQuantizeStatus::InvalidTempo;
        return result;
    }

    const auto rounded = std::llround(frames);
    if (rounded < 0 || rounded > static_cast<long long>(context.num_samples)) {
        result.status = TransportQuantizeStatus::OutsideBlock;
        return result;
    }

    result.scheduled = true;
    result.block_offset = static_cast<std::uint32_t>(rounded);
    result.status = TransportQuantizeStatus::Scheduled;
    return result;
}

bool TransportQuantizer::detect_transport_jump(const ProcessContext& context,
                                               bool timeline_valid) const noexcept {
    if (!last_.valid || !timeline_valid || !last_.is_playing || !context.is_playing) {
        return false;
    }

    auto expected = last_.position_beats +
                    frames_to_beats(static_cast<double>(last_.num_samples),
                                    last_.sample_rate,
                                    last_.tempo_bpm);
    if (last_.is_looping && context.is_looping &&
        valid_loop_range(last_.loop_start_beats, last_.loop_end_beats) &&
        valid_loop_range(context.loop_start_beats, context.loop_end_beats) &&
        std::abs(last_.loop_start_beats - context.loop_start_beats) <=
            timebase::kBoundaryEpsilonBeats &&
        std::abs(last_.loop_end_beats - context.loop_end_beats) <=
            timebase::kBoundaryEpsilonBeats) {
        expected = wrap_loop_position(expected,
                                      context.loop_start_beats,
                                      context.loop_end_beats);
    }

    return std::abs(context.position_beats - expected) >
           timeline_tolerance_beats(context);
}

void TransportQuantizer::remember(const ProcessContext& context,
                                  bool timeline_valid) noexcept {
    if (!timeline_valid) {
        last_.valid = false;
        return;
    }
    last_.valid = true;
    last_.is_playing = context.is_playing;
    last_.is_looping = context.is_looping;
    last_.sample_rate = context.sample_rate;
    last_.tempo_bpm = context.tempo_bpm;
    last_.position_beats = context.position_beats;
    last_.loop_start_beats = context.loop_start_beats;
    last_.loop_end_beats = context.loop_end_beats;
    last_.num_samples = context.num_samples;
}

}  // namespace pulp::format
