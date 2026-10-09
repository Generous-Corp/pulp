#pragma once

#include <cstdint>

namespace pulp::host {

// A control-side receipt for the graph's authoring state.  `graph_identity`
// identifies one SignalGraph lifetime and is deliberately process-local;
// `generation` advances whenever authoring state changes, including a clear()
// that may recycle compact NodeIds.  Consumers can retain a receipt while
// preparing an imported change and reject it if another control-side writer
// changed the graph before the change is committed.  The receipt never enters
// the audio callback or the serialized .pulpgraph format.
struct GraphAuthoringReceipt {
    std::uint64_t graph_identity = 0;
    std::uint64_t generation = 0;

    bool valid() const noexcept {
        return graph_identity != 0;
    }

    friend bool operator==(const GraphAuthoringReceipt&, const GraphAuthoringReceipt&) = default;
};

enum class GraphAuthoringReceiptStatus : std::uint8_t {
    Current,
    WrongGraph,
    Stale,
};

} // namespace pulp::host
