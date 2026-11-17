#include <pulp/host/host_timeline_binding.hpp>

#include <pulp/host/signal_graph_runtime.hpp>

#include <algorithm>
#include <limits>

namespace pulp::host {

namespace {

HostEventResult from_trigger(TriggerGraphResult result) noexcept {
    switch (result) {
    case TriggerGraphResult::Accepted:
        return HostEventResult::Accepted;
    case TriggerGraphResult::Busy:
        return HostEventResult::Busy;
    case TriggerGraphResult::ClockExhausted:
        return HostEventResult::Overflow;
    case TriggerGraphResult::Binding:
        return HostEventResult::Unbound;
    default:
        return HostEventResult::StaleEpoch;
    }
}

bool parameter_less(const HostTimelineParameterRecord& lhs,
                    const HostTimelineParameterRecord& rhs) noexcept {
    if (lhs.absolute_sample != rhs.absolute_sample)
        return lhs.absolute_sample < rhs.absolute_sample;
    if (lhs.producer != rhs.producer)
        return lhs.producer < rhs.producer;
    return lhs.producer_sequence < rhs.producer_sequence;
}

// The graph refused before it consumed the block: its host ingress block is
// still open, so the binding block stays open for a retry.
bool graph_refused_before_projection(TriggerGraphResult result) noexcept {
    return result != TriggerGraphResult::Accepted &&
           result != TriggerGraphResult::ProcessingFailure &&
           result != TriggerGraphResult::StaleGeneration;
}

} // namespace

HostTimelineBinding::HostTimelineBinding(SignalGraph* graph, VoiceBankBinding* voices) noexcept
    : graph_(graph), voices_(voices) {}

HostEventResult HostTimelineBinding::begin_block(const HostBlockClock& clock,
                                                 HostTimelineBlock& out) noexcept {
    if (open_)
        return HostEventResult::Busy;
    const auto refuse_block = [this](HostEventResult result) noexcept {
        ++counters_.refused_blocks;
        return result;
    };

    // Stage the timeline step on a copy: nothing below commits it until every
    // fallible step of the reset transaction has succeeded.
    HostBlockTimeline staged = timeline_;
    if (reset_requested_ && has_block_) {
        const auto epoch = timeline_.current().epoch;
        if (epoch == std::numeric_limits<std::uint64_t>::max())
            return refuse_block(HostEventResult::Overflow);
        staged.reset(epoch + 1);
    }
    HostBlockWindow window;
    if (const auto result = staged.advance(clock, window); result != HostEventResult::Accepted)
        return refuse_block(result);
    const auto boundary = staged.last_boundary();
    const bool reset = reset_requested_ || !has_block_ || boundary == HostBlockBoundary::First ||
                       boundary == HostBlockBoundary::NewEpoch;

    std::uint64_t voice_epoch = voices_ != nullptr ? voices_->epoch() : 0;
    if (reset) {
        // Reserve before the graph reset: a reservation changes no voice
        // state, so a graph refusal below leaves every family untouched.
        if (voices_ != nullptr) {
            if (const auto result = voices_->reserve_event_reset(voice_epoch);
                result != HostEventResult::Accepted) {
                ++counters_.refused_resets;
                return refuse_block(result);
            }
        }
        if (graph_ != nullptr) {
            if (const auto result = graph_->try_reset_events();
                result != TriggerGraphResult::Accepted) {
                ++counters_.refused_resets;
                return refuse_block(from_trigger(result));
            }
        }
        // Infallible commits: the voice bank has no open block (the reserve
        // proved it and only this owner opens one) and holds the reservation.
        if (voices_ != nullptr)
            (void)voices_->commit_event_reset(voice_epoch);
        counters_.parameters_reset += parameter_count_;
        parameter_count_ = 0;
        reset_requested_ = false;
        ++counters_.resets;
    } else if (boundary == HostBlockBoundary::Gap) {
        evict_parameters_before_(window.sample_start);
    }
    timeline_ = staged;
    has_block_ = true;

    HostTimelineBlock block;
    block.timeline = window;
    block.boundary = boundary;
    block.reset = reset;
    block.voice_epoch = voice_epoch;

    // From here a refusal consumes the block on the timeline as a skipped
    // block; the transaction above is already complete and coherent.
    TriggerCounters graph_clock;
    if (graph_ != nullptr) {
        if (const auto result = graph_->trigger_counters(graph_clock);
            result != TriggerGraphResult::Accepted)
            return refuse_block(from_trigger(result));
        if (!reset && mapped_graph_epoch_ != 0 && graph_clock.epoch != mapped_graph_epoch_)
            ++counters_.graph_remaps;
        mapped_graph_epoch_ = graph_clock.epoch;
        block.graph_epoch = graph_clock.epoch;
        block.graph_sample_start = graph_clock.cursor;
        // A failed block's records could not be dropped when it closed; drop
        // them before this block opens so they are not delivered here.
        if (graph_discard_pending_) {
            const auto result = graph_->reset_host_event_ingress(graph_clock.epoch);
            if (result == HostEventResult::Busy)
                return refuse_block(result);
            graph_discard_pending_ = false;
        }
    }
    if (voices_ != nullptr) {
        if (const auto result = voices_->begin_block(window.sample_start, window.frames);
            result != HostEventResult::Accepted)
            return refuse_block(result);
    }
    if (graph_ != nullptr) {
        if (const auto result = graph_->begin_host_event_block(graph_clock.epoch,
                                                               graph_clock.cursor, window.frames);
            result != HostEventResult::Accepted) {
            if (voices_ != nullptr)
                (void)voices_->abort_block();
            return refuse_block(result);
        }
    }
    block_ = block;
    open_ = true;
    ++counters_.blocks;
    out = block_;
    return HostEventResult::Accepted;
}

HostEventResult HostTimelineBinding::check_window_(const HostEventRecord& record) const noexcept {
    if (!open_ || record.epoch != block_.timeline.epoch)
        return HostEventResult::StaleEpoch;
    if (record.producer == 0)
        return HostEventResult::Unbound;
    if (record.absolute_sample < 0)
        return HostEventResult::OutOfRange;
    if (static_cast<std::uint64_t>(record.absolute_sample) < block_.timeline.sample_start)
        return HostEventResult::Late;
    return HostEventResult::Accepted;
}

HostEventResult HostTimelineBinding::publish_graph_event(const HostEventRecord& record) noexcept {
    if (graph_ == nullptr)
        return HostEventResult::Unbound;
    if (const auto result = check_window_(record); result != HostEventResult::Accepted)
        return result;
    const auto offset =
        static_cast<std::uint64_t>(record.absolute_sample) - block_.timeline.sample_start;
    if (offset >= block_.timeline.frames)
        return HostEventResult::OutOfRange;
    constexpr auto kMaxAbsolute =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (block_.graph_sample_start > kMaxAbsolute - offset)
        return HostEventResult::OutOfRange;
    HostEventRecord mapped = record;
    mapped.epoch = block_.graph_epoch;
    mapped.absolute_sample = static_cast<std::int64_t>(block_.graph_sample_start + offset);
    mapped.midi.sample_offset = static_cast<std::int32_t>(offset);
    return graph_->publish_host_event(mapped);
}

HostEventResult HostTimelineBinding::publish_voice_event(const HostEventRecord& record) noexcept {
    if (voices_ == nullptr)
        return HostEventResult::Unbound;
    if (const auto result = check_window_(record); result != HostEventResult::Accepted)
        return result;
    HostEventRecord mapped = record;
    mapped.epoch = block_.voice_epoch;
    return voices_->publish(mapped);
}

HostEventResult
HostTimelineBinding::publish_parameter(const HostTimelineParameterRecord& record) noexcept {
    const auto refuse = [this](HostEventResult result) noexcept {
        ++counters_.parameters_refused;
        if (result == HostEventResult::Overflow)
            ++counters_.parameters_overflow;
        return result;
    };
    if (!open_ || record.epoch != block_.timeline.epoch)
        return refuse(HostEventResult::StaleEpoch);
    if (record.producer == 0)
        return refuse(HostEventResult::Unbound);
    if (record.absolute_sample < 0)
        return refuse(HostEventResult::OutOfRange);
    if (static_cast<std::uint64_t>(record.absolute_sample) < block_.timeline.sample_start)
        return refuse(HostEventResult::Late);
    for (std::size_t i = 0; i < parameter_count_; ++i)
        if (parameters_[i].producer == record.producer &&
            parameters_[i].producer_sequence == record.producer_sequence)
            return refuse(HostEventResult::InvalidSequence);
    if (parameter_count_ == parameters_.size())
        return refuse(HostEventResult::Overflow);
    parameters_[parameter_count_++] = record;
    ++counters_.parameters_accepted;
    return HostEventResult::Accepted;
}

HostEventResult HostTimelineBinding::take_parameters(state::ParameterEventQueue& out) noexcept {
    if (!open_)
        return HostEventResult::StaleEpoch;
    const auto start = block_.timeline.sample_start;
    const auto end = start + block_.timeline.frames;
    std::size_t matching = 0;
    for (std::size_t i = 0; i < parameter_count_; ++i) {
        const auto sample = static_cast<std::uint64_t>(parameters_[i].absolute_sample);
        if (sample >= start && sample < end)
            parameter_order_[matching++] = i;
    }
    if (matching > out.capacity() - (out.size() < out.capacity() ? out.size() : out.capacity()))
        return HostEventResult::OutputCapacity;
    for (std::size_t i = 1; i < matching; ++i) {
        const auto value = parameter_order_[i];
        std::size_t j = i;
        while (j > 0 && parameter_less(parameters_[value], parameters_[parameter_order_[j - 1]])) {
            parameter_order_[j] = parameter_order_[j - 1];
            --j;
        }
        parameter_order_[j] = value;
    }
    for (std::size_t i = 0; i < matching; ++i) {
        const auto& record = parameters_[parameter_order_[i]];
        state::ParameterEvent event;
        event.param_id = record.param_id;
        event.sample_offset =
            static_cast<std::int32_t>(static_cast<std::uint64_t>(record.absolute_sample) - start);
        event.value = record.value;
        (void)out.push(event);
    }
    std::size_t keep = 0;
    for (std::size_t i = 0; i < parameter_count_; ++i) {
        const auto sample = static_cast<std::uint64_t>(parameters_[i].absolute_sample);
        if (sample >= end)
            parameters_[keep++] = parameters_[i];
    }
    parameter_count_ = keep;
    return HostEventResult::Accepted;
}

HostTimelineRenderResult
HostTimelineBinding::render(audio::BufferView<float>* graph_output,
                            const audio::BufferView<const float>* graph_input,
                            audio::BufferView<float>* voice_stereo) noexcept {
    HostTimelineRenderResult result;
    const auto frames = block_.timeline.frames;
    const bool graph_args = graph_output != nullptr && graph_input != nullptr;
    const bool graph_args_ok = graph_ == nullptr
                                   ? graph_output == nullptr && graph_input == nullptr
                                   : graph_args && graph_output->num_samples() >= frames &&
                                         graph_input->num_samples() >= frames;
    const bool voice_args_ok = voices_ == nullptr
                                   ? voice_stereo == nullptr
                                   : voice_stereo != nullptr && voice_stereo->num_channels() == 2 &&
                                         voice_stereo->num_samples() == frames;
    // A refused call leaves the block open so it can be rendered correctly.
    if (!open_ || !graph_args_ok || !voice_args_ok) {
        result.graph = TriggerGraphResult::Binding;
        result.voice = VoiceBankBindingCode::InvalidBlock;
        return result;
    }
    if (graph_ != nullptr) {
        result.graph =
            graph_->process_checked(*graph_output, *graph_input, static_cast<int>(frames));
        if (graph_refused_before_projection(result.graph))
            return result;
        if (result.graph == TriggerGraphResult::ProcessingFailure)
            drop_failed_graph_block_(*graph_output);
    }
    if (voices_ != nullptr) {
        result.voice = voices_->render(*voice_stereo, block_.timeline.sample_start, frames);
        if (result.voice == VoiceBankBindingCode::InvalidBlock)
            (void)voices_->abort_block();
    }
    // Parameter records of this block that the caller did not take are not
    // carried into the next block.
    const auto end = block_.timeline.sample_start + frames;
    std::size_t keep = 0;
    for (std::size_t i = 0; i < parameter_count_; ++i)
        if (static_cast<std::uint64_t>(parameters_[i].absolute_sample) >= end)
            parameters_[keep++] = parameters_[i];
    counters_.parameters_skipped += parameter_count_ - keep;
    parameter_count_ = keep;
    open_ = false;
    return result;
}

void HostTimelineBinding::drop_failed_graph_block_(audio::BufferView<float>& output) noexcept {
    ++counters_.graph_failed_blocks;
    // A refused projection restores the block's records and leaves the graph
    // cursor where it was, so a cursor that has not moved means the graph
    // rendered nothing: hand the caller silence instead of a stale buffer.
    TriggerCounters clock;
    if (graph_->trigger_counters(clock) == TriggerGraphResult::Accepted &&
        clock.epoch == block_.graph_epoch && clock.cursor == block_.graph_sample_start) {
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
            std::fill_n(output.channel_ptr(channel), block_.timeline.frames, 0.0f);
    }
    // Retrying the restored records fails the same way, and the next block
    // maps onto the same graph cursor, so keeping them would replay them a
    // block late and stack them onto the next block's records. The binding
    // only ever stages graph records inside the current block, so clearing
    // the ingress drops exactly this block's records.
    graph_discard_pending_ =
        graph_->reset_host_event_ingress(block_.graph_epoch) == HostEventResult::Busy;
}

void HostTimelineBinding::evict_parameters_before_(std::uint64_t sample) noexcept {
    std::size_t keep = 0;
    for (std::size_t i = 0; i < parameter_count_; ++i)
        if (static_cast<std::uint64_t>(parameters_[i].absolute_sample) >= sample)
            parameters_[keep++] = parameters_[i];
    counters_.parameters_skipped += parameter_count_ - keep;
    parameter_count_ = keep;
}

} // namespace pulp::host
