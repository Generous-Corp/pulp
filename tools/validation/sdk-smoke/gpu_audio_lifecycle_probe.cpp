#include <pulp/audio/buffer.hpp>
#include <pulp/gpu_audio/gpu_audio_node.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>

#include <algorithm>
#include <cstdint>

namespace {

using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::gpu_audio::GpuAudioNode;
using pulp::gpu_audio::GpuAudioNodeDescriptor;
using pulp::gpu_audio::GpuAudioTransport;
using pulp::gpu_audio::MissPolicy;

class LifecycleNode final : public GpuAudioNode {
  public:
    static constexpr std::uint32_t kChannels = 1;
    static constexpr std::uint32_t kBlock = 32;
    static constexpr std::uint32_t kLatency = 2;

    GpuAudioNodeDescriptor descriptor() const override {
        return {.name = "installed-sdk-lifecycle-probe",
                .input_channels = kChannels,
                .output_channels = kChannels,
                .block_size = kBlock,
                .sample_rate = 48'000,
                .latency_blocks = kLatency,
                .miss_policy = MissPolicy::CpuFallback,
                .supports_cpu_fallback = true};
    }

    bool prepare() override {
        ++prepare_calls;
        return true;
    }

    void process_block(const BufferView<const float>& input, BufferView<float>& output,
                       std::uint32_t n) override {
        ++gpu_like_process_calls;
        for (std::uint32_t i = 0; i < n; ++i)
            output.channel_ptr(0)[i] = input.channel_ptr(0)[i] * 2.0f;
    }

    void process_cpu_fallback(const BufferView<const float>& input, BufferView<float>& output,
                              std::uint32_t n) noexcept override {
        ++fallback_calls;
        for (std::uint32_t i = 0; i < n; ++i)
            output.channel_ptr(0)[i] = -input.channel_ptr(0)[i];
    }

    std::uint32_t prepare_calls = 0;
    std::uint32_t gpu_like_process_calls = 0;
    std::uint32_t fallback_calls = 0;
};

bool check(bool value) {
    return value;
}

} // namespace

int main() {
    LifecycleNode node;
    if (!check(node.prepare()))
        return 10;

    Buffer<float> input(1, LifecycleNode::kBlock);
    Buffer<float> output(1, LifecycleNode::kBlock);
    GpuAudioTransport transport;
    if (!check(transport.prepare(&node, {.ring_blocks = 8})))
        return 11;

    // Normal lifecycle: process, service the worker, then observe the fixed
    // latency. This is the installed consumer's public API path.
    for (std::uint32_t block = 1; block <= 4; ++block) {
        std::fill(input.channel(0).begin(), input.channel(0).end(), static_cast<float>(block));
        auto input_view = static_cast<const Buffer<float>&>(input).view();
        auto output_view = output.view();
        transport.process(input_view, output_view, LifecycleNode::kBlock);
        transport.pump();
        if (block > LifecycleNode::kLatency &&
            output.channel(0)[0] != static_cast<float>((block - LifecycleNode::kLatency) * 2))
            return 12;
    }
    if (transport.stats().miss_blocks != 0 || node.gpu_like_process_calls != 4)
        return 13;

    // Stop/reprepare lifecycle. release() must quiesce the transport, and a
    // second prepare must start a clean transport without changing node state.
    transport.release();
    if (transport.is_prepared())
        return 14;
    if (!check(transport.prepare(&node, {.ring_blocks = 8})))
        return 15;

    // A fresh prepare primes the declared fixed latency with silence. Consume
    // that latency before checking the no-pump fallback path; otherwise the
    // first calls are intentionally priming output, not worker misses.
    std::fill(input.channel(0).begin(), input.channel(0).end(), 0.0f);
    for (std::uint32_t block = 0; block < LifecycleNode::kLatency; ++block) {
        auto input_view = static_cast<const Buffer<float>&>(input).view();
        auto output_view = output.view();
        transport.process(input_view, output_view, LifecycleNode::kBlock);
        if (output.channel(0)[0] != 0.0f)
            return 151;
    }

    // Missing/late worker proof: with no pump, the public CpuFallback policy
    // must produce a bounded substitute and account for each miss.
    std::fill(input.channel(0).begin(), input.channel(0).end(), 3.0f);
    auto input_view = static_cast<const Buffer<float>&>(input).view();
    auto output_view = output.view();
    transport.process(input_view, output_view, LifecycleNode::kBlock);
    if (output.channel(0)[0] != -3.0f)
        return 16;
    std::fill(input.channel(0).begin(), input.channel(0).end(), 4.0f);
    input_view = static_cast<const Buffer<float>&>(input).view();
    output_view = output.view();
    transport.process(input_view, output_view, LifecycleNode::kBlock);
    if (output.channel(0)[0] != -4.0f)
        return 17;
    if (transport.stats().miss_blocks != 2 || node.fallback_calls < 2)
        return 18;

    // A late worker result is drained after the fallback has already closed
    // the timeline. It must not crash or make the transport unprepared.
    transport.pump();
    if (!transport.is_prepared())
        return 19;

    // The installed public API has no device-loss injection or status hook.
    // Keep this explicit in the receipt rather than pretending the test covers
    // a provider-side loss event. The private Dawn provider probes own that
    // scenario until a public diagnostic contract exists.
    return 0;
}
