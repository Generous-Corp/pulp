#pragma once

#include "streaming_model.hpp"
#include "wavenet_realtime_channel.hpp"

#include <pulp/gpu_audio/gpu_wavenet.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pulp::gpu_audio::detail {

/// StreamingBackend over one GpuWaveNetSession per audio channel. Every
/// method is serialized worker or host work; nothing here is callback-safe.
///
/// The generic neural node retains each admitted block's input and output
/// spans until receive() publishes that block's terminal. Channel results
/// are collated by sequence, so a block completes only when every channel
/// has returned, and terminals are published in admission order.
class WaveNetStreamingBackend final : public StreamingBackend {
  public:
    struct Config {
        GpuWaveNetSession::Config session{};
        std::uint32_t channels = 1;
        std::uint32_t max_inflight = 1;
    };
    /// Builds one provider channel. Returning null fails prepare(); the
    /// production factory wraps GpuWaveNetSession::create().
    using ChannelFactory = std::function<std::unique_ptr<WaveNetRealtimeChannel>(std::uint32_t)>;

    /// Copies descriptors, dilation arrays and weights. Host thread only.
    /// An empty factory selects the authenticated GpuWaveNetSession provider.
    explicit WaveNetStreamingBackend(const Config& config, ChannelFactory factory = {});
    ~WaveNetStreamingBackend() override;
    WaveNetStreamingBackend(const WaveNetStreamingBackend&) = delete;
    WaveNetStreamingBackend& operator=(const WaveNetStreamingBackend&) = delete;

    bool prepare(const StreamingPrepareContext& context) noexcept override;
    std::size_t service_until(std::uint64_t deadline_ns) noexcept override;
    bool receive(StreamingTerminal& terminal) noexcept override;
    bool begin_epoch(std::uint64_t epoch, StreamingResetReason reason) noexcept override;
    // Device-loss reprepare belongs to the recovery steward; until then a
    // lost backend stays released and the node continues on its CPU shadow.
    bool reprepare_after_device_loss(std::uint64_t) noexcept override {
        return false;
    }
    bool quiesce() noexcept override;
    bool release() noexcept override;

    /// True when the channels are authenticated GpuWaveNetSession providers
    /// rather than an injected test seam.
    bool production_provider() const noexcept {
        return production_ && prepared_;
    }
    bool prepared() const noexcept {
        return prepared_;
    }
    std::uint32_t max_inflight() const noexcept {
        return config_.max_inflight;
    }
    std::uint32_t slots() const noexcept {
        return config_.session.slots;
    }

  protected:
    StreamingAdmission enqueue_validated(const StreamingBlock& block) noexcept override;

  private:
    struct Slot {
        StreamingBlockStamp stamp{};
        std::span<float> output{};
        std::uint32_t output_channel_stride = 0;
        std::uint32_t output_frame_stride = 1;
        std::uint64_t completed_mask = 0;
        bool occupied = false;
        bool failed = false;
        bool late = false;
    };
    Slot* find(std::uint64_t sequence) noexcept;
    void fail_all_pending() noexcept;
    bool release_channels() noexcept;

    Config config_;
    ChannelFactory factory_;
    bool production_ = false;
    std::vector<GpuWaveNetLayerDescriptor> layers_;
    std::vector<std::vector<std::uint32_t>> dilations_;
    std::vector<float> weights_;
    std::vector<std::unique_ptr<WaveNetRealtimeChannel>> channels_;
    std::vector<Slot> slots_;
    std::vector<float> scratch_;
    std::size_t head_ = 0, count_ = 0;
    std::uint64_t epoch_ = 0;
    std::uint64_t all_channels_mask_ = 0;
    std::uint32_t block_size_ = 0;
    bool prepared_ = false;
};

} // namespace pulp::gpu_audio::detail
