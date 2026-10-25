#pragma once

#include <atomic>

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

// A pipelining node's opt-in, shared with the transport that hosts it. The
// node owns one for its whole life and detaches it as the first step of its
// destructor, so a transport released after the node is destroyed reaches
// only the link, never the node. Detaching is one atomic store: it never
// blocks, and never waits on the audio thread. Hosting changes and node
// destruction are host-thread operations the host does not run concurrently.
class WorkerPipelineLink {
  public:
    using SetFn = bool (*)(void* node, bool hosted) noexcept;
    WorkerPipelineLink(void* node, SetFn set) noexcept : node_(node), set_(set) {}

    bool set_hosting(bool hosted) noexcept {
        auto* node = node_.load(std::memory_order_acquire);
        return node != nullptr && set_(node, hosted);
    }
    void detach() noexcept {
        node_.store(nullptr, std::memory_order_release);
    }
    bool attached() const noexcept {
        return node_.load(std::memory_order_acquire) != nullptr;
    }

  private:
    std::atomic<void*> node_;
    const SetFn set_;
};

} // namespace pulp::gpu_audio::detail
