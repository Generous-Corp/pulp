#pragma once

/// @file custom_node_events.hpp
/// The event block a Custom graph node receives, and the callback types that
/// take it.
///
/// The graph has routed MIDI for a long time: `gather_node_midi` fans inbound
/// events into per-node scratch for *every* node before the executor dispatches
/// on binding kind, and a `Processor` wrapped by `add_processor_node` already
/// consumes them. A Custom node was the one graph citizen whose binding never
/// handed that scratch on. These types complete that lane; they do not add MIDI
/// to the graph.
///
/// The block is a struct rather than a pair of arguments so that a later
/// transport-aware or automation-aware event node is a new *member* here, not a
/// fourth callback pair on `CustomNodeType`. It lives in its own header because
/// `custom_node_type.hpp` is a reviewed public surface whose byte hash gates the
/// capability fingerprint; keeping the vocabulary out of it holds that file's
/// growth to the two callback members themselves.

#include <pulp/audio/buffer.hpp>

#include <functional>

namespace pulp {
namespace midi {
class MidiBuffer;
}
}

namespace pulp::host {

/// The events visible to one Custom node for one block.
///
/// `in` is the node's gathered inbound MIDI. A null `in` means "no events this
/// block" and is not an error: the routed path only allocates an event input
/// port when the compiled shape carries MIDI, so an audio-only graph hands the
/// callback nothing while the reference walk hands it an empty buffer. A
/// conforming callback must treat the two as identical, which is why the
/// parity test compares rendered audio rather than buffer identity.
///
/// `out` is the node's own MIDI output buffer: a real, writable destination that
/// downstream nodes gather from, cleared by the caller immediately before
/// dispatch so nothing a previous block emitted survives into this one. It is
/// nullable on exactly the same terms as `in` — the routed path's per-node
/// scratch is null when the compiled shape carries no MIDI at all (an audio-only
/// graph), while the reference walk always hands over its per-node buffer — so a
/// conforming callback must null-check it and treat a null as "this graph has no
/// event lane", never as an error. Appends respect the buffer's realtime
/// capacity: an `add()` past capacity is dropped and recorded, and the executor
/// derives this node's `out_incomplete` from those drops after the callback
/// returns, exactly as it does for a plugin node.
struct CustomNodeEventBlock {
    const midi::MidiBuffer* in = nullptr;
    midi::MidiBuffer* out = nullptr;
};

/// Stateless event-aware process callback.
using CustomNodeEventProcessFn =
    std::function<void(audio::BufferView<float>& output,
                       const audio::BufferView<const float>& input, int num_samples,
                       const CustomNodeEventBlock& events)>;

/// Stateful event-aware process callback. The first argument is the instance
/// returned by the type's `create`, exactly as `process_instance` receives it.
using CustomNodeInstanceEventProcessFn =
    std::function<void(void* instance, audio::BufferView<float>& output,
                       const audio::BufferView<const float>& input, int num_samples,
                       const CustomNodeEventBlock& events)>;

}  // namespace pulp::host
