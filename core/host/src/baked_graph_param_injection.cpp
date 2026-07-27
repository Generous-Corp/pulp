#include <pulp/host/baked_graph_processor.hpp>
#include "baked_graph_processor_detail.hpp"

#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/state/param_cursor.hpp>
#include <pulp/state/parameter.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <utility>

namespace pulp::host {
namespace {

class CursorParamView final : public BakedParamView {
public:
    explicit CursorParamView(state::ParamCursor& cursor) noexcept : cursor_(cursor) {}

    float value_at(state::ParamID id, std::int32_t sample_offset) const override {
        assert(sample_offset >= last_offset_ &&
               "BakedParamView::value_at offsets must be non-decreasing within a block");
        last_offset_ = sample_offset;
        cursor_.advance_to(sample_offset);
        return cursor_.value(id);
    }

    float value(state::ParamID id) const override { return cursor_.value(id); }

private:
    state::ParamCursor& cursor_;
    mutable std::int32_t last_offset_ = std::numeric_limits<std::int32_t>::min();
};

}  // namespace

void BakedGraphProcessor::prepare_param_injection() {
    for (auto& [node_id, runtime] : custom_nodes_) {
        auto& binding = runtime->params;
        runtime->param_state.reset();
        if (binding.params.empty() || !binding.process || !runtime->mailbox) continue;

        auto state = std::make_unique<detail::BakedParamNodeState>();
        state->process = binding.process;
        state->mailbox = runtime->mailbox.get();
        state->current.reserve(binding.params.size());
        for (const auto& param : binding.params) {
            state::ParamInfo info;
            info.id = param.id;
            info.name = "baked_param_" + std::to_string(param.id);
            info.range.min = param.min_value;
            info.range.max = param.max_value;
            info.range.default_value = param.default_value;
            state->store.add_parameter(info);
            state->current.push_back(state::ParamSnapshotEntry{
                param.id,
                std::clamp(param.default_value, param.min_value, param.max_value)});
        }
        state->ramp_carry.resize(state->current.size());

        auto* rt = state.get();
        runtime->param_state = std::move(state);
        runtime->process =
            [rt](pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in, int frames) {
                pulp::runtime::ScopedNoAlloc no_alloc;
                rt->scratch.clear();

                for (std::size_t i = 0; i < rt->current.size(); ++i) {
                    const auto& carry = rt->ramp_carry[i];
                    if (!carry.ramping || carry.remaining <= 0) continue;
                    rt->scratch.push(state::ParameterEvent{
                        rt->current[i].param_id, 0, carry.target, carry.remaining});
                }

                const auto& snapshot = rt->mailbox->published.read();
                if (snapshot.sequence > rt->sequence_seen) {
                    for (std::size_t i = 0; i < snapshot.size; ++i) {
                        rt->scratch.push(snapshot.events[i]);
                    }
                    rt->sequence_seen = snapshot.sequence;
                    rt->mailbox->consumed_sequence.store(snapshot.sequence,
                                                         std::memory_order_release);
                }
                rt->scratch.sort();

                state::ParamCursor cursor(rt->store, &rt->scratch, rt->current);
                CursorParamView view(cursor);
                rt->process(out, in, frames, view);

                if (frames <= 0) return;
                cursor.advance_to(frames);
                for (std::size_t i = 0; i < rt->current.size(); ++i) {
                    const auto id = rt->current[i].param_id;
                    rt->current[i].value = cursor.value(id);
                    auto& carry = rt->ramp_carry[i];
                    if (cursor.is_ramping(id)) {
                        carry.ramping = true;
                        carry.target = cursor.ramp_target(id);
                        carry.remaining = cursor.ramp_end_offset(id) - frames;
                        if (carry.remaining <= 0) carry.ramping = false;
                    } else {
                        carry.ramping = false;
                        carry.remaining = 0;
                    }
                }
            };
    }
}

ParamInjector BakedGraphProcessor::claim_param_injection(NodeId node) noexcept {
    const auto it = custom_nodes_.find(node);
    if (it == custom_nodes_.end() || !it->second->mailbox) return ParamInjector{};
    const auto& mailbox = it->second->mailbox;
    bool expected = false;
    if (!mailbox->claimed.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return ParamInjector{};
    }
    return ParamInjector{mailbox};
}

ParamInjector::ParamInjector(
    std::shared_ptr<detail::BakedParamMailbox> mailbox) noexcept
    : mailbox_(std::move(mailbox)) {}

ParamInjector::~ParamInjector() { release(); }

ParamInjector::ParamInjector(ParamInjector&& other) noexcept
    : mailbox_(std::move(other.mailbox_)) {}

ParamInjector& ParamInjector::operator=(ParamInjector&& other) noexcept {
    if (this != &other) {
        release();
        mailbox_ = std::move(other.mailbox_);
    }
    return *this;
}

void ParamInjector::release() noexcept {
    if (!mailbox_) return;
    mailbox_->claimed.store(false, std::memory_order_release);
    mailbox_.reset();
}

InjectStatus ParamInjector::inject(const state::ParameterEventQueue& events) noexcept {
    if (!mailbox_) return InjectStatus::InvalidHandle;
    auto& scratch = mailbox_->writer_scratch;
    scratch.size = 0;
    for (const auto& event : events.events()) {
        if (scratch.size >= scratch.events.size()) break;
        scratch.events[scratch.size++] = event;
    }
    scratch.sequence =
        mailbox_->next_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    mailbox_->published.write(scratch);
    return events.overflowed() ? InjectStatus::PartialOverflow : InjectStatus::Ok;
}

InjectStatus ParamInjector::inject(const state::ParameterEvent& event) noexcept {
    if (!mailbox_) return InjectStatus::InvalidHandle;
    auto& scratch = mailbox_->writer_scratch;
    if (mailbox_->consumed_sequence.load(std::memory_order_acquire) >= scratch.sequence) {
        scratch.size = 0;
    }
    std::size_t write = 0;
    for (std::size_t read = 0; read < scratch.size; ++read) {
        if (scratch.events[read].param_id != event.param_id) {
            scratch.events[write++] = scratch.events[read];
        }
    }
    scratch.size = write;
    if (scratch.size >= scratch.events.size()) return InjectStatus::PartialOverflow;
    scratch.events[scratch.size++] = event;
    scratch.sequence =
        mailbox_->next_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    mailbox_->published.write(scratch);
    return InjectStatus::Ok;
}

}  // namespace pulp::host
