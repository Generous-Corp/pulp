#pragma once

namespace pulp::gpu_audio {
class GpuAudioNode;
}

namespace pulp::gpu_audio::detail {

// Transport-side opt-in to worker pipelining. GpuAudioTransport sets `hosted`
// before it reads the node's descriptor at prepare() and clears it at
// release(), with no worker running in either case. A node that supports a
// worker pipeline (today the staged GpuConvolver) then declares
// worker_pipeline_blocks and emits block N - D; any other node, and the same
// node called directly, keeps block-in, block-out semantics. A node takes part
// by binding RealtimeGpuNodeBinding::pipeline_hosting in bind_realtime_path();
// this dispatches through that binding. Returns false for a node that does not
// take part.
bool set_worker_pipeline_hosting(GpuAudioNode* node, bool hosted) noexcept;

} // namespace pulp::gpu_audio::detail
