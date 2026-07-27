#pragma once

#include <pulp/host/baked_graph_processor.hpp>

#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/state/param_cursor.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/state/store.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::host::detail {

struct BakedParamBlockSnapshot {
    std::array<state::ParameterEvent, state::ParameterEventQueue::kCapacity> events{};
    std::size_t size = 0;
    std::uint64_t sequence = 0;
};

struct BakedParamMailbox {
    runtime::TripleBuffer<BakedParamBlockSnapshot> published;
    BakedParamBlockSnapshot writer_scratch;
    std::atomic<std::uint64_t> next_sequence{0};
    std::atomic<std::uint64_t> consumed_sequence{0};
    std::atomic<bool> claimed{false};
};

struct BakedParamRampCarry {
    bool ramping = false;
    float target = 0.0f;
    std::int32_t remaining = 0;
};

struct BakedParamNodeState {
    state::StateStore store;
    state::ParameterEventQueue scratch;
    std::vector<state::ParamSnapshotEntry> current;
    std::vector<BakedParamRampCarry> ramp_carry;
    std::uint64_t sequence_seen = 0;
    CustomNodeParamProcessFn process;
    BakedParamMailbox* mailbox = nullptr;
};

struct BakedCustomNodeRuntime {
    CustomNodeProcessFn process;
    CustomNodeLifecycle lifecycle;
    BakedCustomParamBinding params;
    std::function<int(double)> latency_samples;
    std::shared_ptr<BakedParamMailbox> mailbox;
    std::unique_ptr<BakedParamNodeState> param_state;
};

}  // namespace pulp::host::detail
