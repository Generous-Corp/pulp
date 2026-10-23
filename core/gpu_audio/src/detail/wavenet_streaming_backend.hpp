#pragma once

#include "streaming_model.hpp"
#include "wavenet_realtime_channel.hpp"

#include <pulp/gpu_audio/gpu_wavenet.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pulp::gpu_audio::detail {

/// Provider channel over one session. `Session` has GpuWaveNetSession's
/// submit/service/receive/release/device_lost surface.
template <class Session> class SessionChannel final : public WaveNetRealtimeChannel {
  public:
    explicit SessionChannel(std::unique_ptr<Session> session) : session_(std::move(session)) {}
    bool submit(std::span<const float> input, std::uint64_t sequence) noexcept override {
        return session_->submit_block(input, sequence);
    }
    void service(std::uint64_t now) noexcept override {
        session_->service(now);
    }
    void service_until(std::uint64_t, std::uint64_t deadline) noexcept override {
        session_->service_until(deadline);
    }
    std::optional<GpuWaveNetBlockResult> receive(std::span<float> output) noexcept override {
        return session_->receive(output);
    }
    bool release() noexcept override {
        return session_->release();
    }
    bool lost() const noexcept override {
        return session_->device_lost();
    }

  private:
    std::unique_ptr<Session> session_;
};

/// Creates one session and appends it to `channels`. A partially prepared
/// session is appended too, so release() can retry its physical drain.
/// Returns whether creation succeeded. Host thread; may throw on allocation.
template <class Session>
bool create_session_channel(const GpuWaveNetSession::Config& config,
                            std::vector<std::unique_ptr<WaveNetRealtimeChannel>>& channels) {
    auto result = Session::create(config);
    // Read the outcome before the session moves out: the result's
    // operator bool also requires the session pointer.
    const bool created = static_cast<bool>(result);
    if (result.session)
        channels.push_back(std::make_unique<SessionChannel<Session>>(std::move(result.session)));
    return created;
}

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
    /// Creates one session channel; see create_session_channel().
    using SessionCreator = bool (*)(const GpuWaveNetSession::Config&,
                                    std::vector<std::unique_ptr<WaveNetRealtimeChannel>>&);

    /// Copies descriptors, dilation arrays and weights. Host thread only.
    /// An empty factory selects the authenticated GpuWaveNetSession provider.
    /// Tests may substitute `creator` to run the session path without a GPU;
    /// the backend is then not an authenticated provider.
    explicit WaveNetStreamingBackend(const Config& config, ChannelFactory factory = {},
                                     SessionCreator creator = nullptr);
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
    bool device_lost() const noexcept override {
        return lost_;
    }

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
    bool any_channel_lost() const noexcept;
    void fail_all_pending() noexcept;
    bool release_channels() noexcept;

    Config config_;
    ChannelFactory factory_;
    SessionCreator creator_ = nullptr;
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
    bool lost_ = false; // worker only; latched until release()
};

} // namespace pulp::gpu_audio::detail
