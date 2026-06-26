#include <pulp/format/processor_node_adapter.hpp>

namespace pulp::format {

bool ProcessorNode::prepare(const PrepareContext& context) {
    if (processor_ == nullptr) return false;
    processor_->prepare(context);
    // Pre-size the discard MIDI sinks so the per-block clear() the bridge runs
    // on the unused fallback buffers never reallocates on the RT thread. The
    // audio-only binding presents no real events, so a small reservation is
    // enough to keep capacity stable across blocks.
    const int midi_hint = context.resource_limits.max_midi_events;
    const auto reserve = static_cast<std::size_t>(midi_hint > 0 ? midi_hint : 1);
    scratch_.fallback_midi_in.reserve_events(reserve);
    scratch_.fallback_midi_out.reserve_events(reserve);
    return true;
}

bool ProcessorNode::process_binding(ProcessBlock& block,
                                    const GraphRuntimeNodeProcessContext& ctx,
                                    void* user_data) noexcept {
    auto* self = static_cast<ProcessorNode*>(user_data);
    if (self == nullptr || self->processor_ == nullptr) return false;
    // This binding only implements the routed contract: it reads the gathered
    // per-node input and writes the per-node output. The shared-block path does
    // not populate those views, so refuse it rather than silently misread the
    // block's buses.
    if (!ctx.routed) return false;

    // Stack-local single-bus set aliasing the executor's routed mono views.
    // BusBufferSet stores its buses inline (no heap), and BufferView only holds
    // the executor-owned channel-pointer array, so this aliases without copying
    // samples and stays allocation-free.
    BusBufferSet local_buses;
    if (!local_buses.add_input("main", ctx.node_inputs, BusRole::Main)) return false;
    if (!local_buses.add_output("main", ctx.node_outputs, BusRole::Main)) return false;

    ProcessBlock local_block;
    local_block.mode = block.mode;
    local_block.flags = block.flags;
    local_block.sample_rate = block.sample_rate;
    local_block.frame_count = block.frame_count;
    local_block.render_speed = block.render_speed;
    local_block.block_index = block.block_index;
    local_block.transport = block.transport;
    local_block.buses = &local_buses;
    // Audio-only binding: no EventBlock, no BlockScratch. The bridge falls back to
    // its scratch MIDI sinks and a null parameter queue.
    if (!local_block.validate()) return false;

    return process_processor_block(*self->processor_, local_block, self->scratch_);
}

} // namespace pulp::format
