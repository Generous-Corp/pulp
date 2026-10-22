#pragma once
#include "shared_io_stamped_bridge.hpp"
#include "shared_io_execution_contract.hpp"
#include "shared_io_provider_identity.hpp"
#include "shared_io_trace.hpp"
#include <pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>
#include <algorithm>
#include <memory>
#include <vector>

namespace pulp::gpu_audio::detail {
// Private access to the provider a GpuWaveNetSession prepared.
struct GpuWaveNetSessionAccess {
    static SharedIoProviderIdentity provider_identity(const GpuWaveNetSession&) noexcept;
};

// A provider completion and the admission ticket of the block it completes.
struct WaveNetChannelCompletion {
    GpuWaveNetBlockResult result;
    // The ticket the block was submitted with. A ticket that is not accepted
    // means the channel cannot attribute the completion to any admission.
    SharedIoSubmissionTicket ticket{};
};

// Private deterministic provider seam. No plugin-facing queue/provider hooks.
// Each block is submitted with its admission ticket and comes back with it, so
// the generation that admitted a block travels with it to its completion.
class WaveNetRealtimeChannel {
  public:
    virtual ~WaveNetRealtimeChannel() = default;
    virtual bool submit(std::span<const float>, const SharedIoSubmissionTicket&) noexcept = 0;
    virtual void service(std::uint64_t) noexcept = 0;
    virtual void service_until(std::uint64_t now_ns, std::uint64_t deadline_ns) noexcept = 0;
    virtual std::optional<WaveNetChannelCompletion> receive(std::span<float>) noexcept = 0;
    virtual bool release() noexcept = 0;
    // Serialized worker diagnostic: the provider's device is gone, so nothing
    // submitted to this channel will complete successfully again.
    virtual bool lost() const noexcept {
        return false;
    }
    // Non-RT: the identity the channel's provider reports for itself.
    virtual SharedIoProviderIdentity provider_identity() const noexcept {
        return {};
    }
};

// Adapts a GpuWaveNetSession, which names a block only by its sequence, to the
// ticketed seam. Admitted tickets wait in a ring indexed by sequence; the
// session holds at most `slots` blocks in flight, so entries cannot collide.
// `Session` has GpuWaveNetSession's submit/service/receive/release/device_lost
// surface, so tests can drive the same adapter over a scripted session.
template <class Session = GpuWaveNetSession>
class GpuWaveNetSessionChannel final : public WaveNetRealtimeChannel {
  public:
    GpuWaveNetSessionChannel(std::unique_ptr<Session> session, std::uint32_t slots)
        : session_(std::move(session)), tickets_(std::max<std::uint32_t>(slots, 1u)) {}
    bool submit(std::span<const float> input,
                const SharedIoSubmissionTicket& ticket) noexcept override {
        if (!ticket.accepted() || !session_->submit_block(input, ticket.sequence))
            return false;
        tickets_[ticket.sequence % tickets_.size()] = ticket;
        return true;
    }
    void service(std::uint64_t now) noexcept override {
        session_->service(now);
    }
    void service_until(std::uint64_t, std::uint64_t deadline) noexcept override {
        session_->service_until(deadline);
    }
    std::optional<WaveNetChannelCompletion> receive(std::span<float> output) noexcept override {
        const auto result = session_->receive(output);
        if (!result)
            return std::nullopt;
        WaveNetChannelCompletion completion{*result, {}};
        auto& entry = tickets_[result->sequence % tickets_.size()];
        if (entry.accepted() && entry.sequence == result->sequence) {
            completion.ticket = entry;
            entry = {};
        }
        return completion;
    }
    bool release() noexcept override {
        std::fill(tickets_.begin(), tickets_.end(), SharedIoSubmissionTicket{});
        return session_->release();
    }
    bool lost() const noexcept override {
        return session_->device_lost();
    }
    SharedIoProviderIdentity provider_identity() const noexcept override {
        return GpuWaveNetSessionAccess::provider_identity(*session_);
    }

  private:
    std::unique_ptr<Session> session_;
    std::vector<SharedIoSubmissionTicket> tickets_;
};
struct WaveNetRealtimeTestAccess {
    static void
    request_recovery(GpuWaveNetRealtimeNode&, SharedIoRecoveryReason,
                     std::uint64_t sequence = std::numeric_limits<std::uint64_t>::max()) noexcept;
    static SharedIoRecoveryReason recovery_reason(const GpuWaveNetRealtimeNode&) noexcept;
    static std::uint64_t recovery_sequence(const GpuWaveNetRealtimeNode&) noexcept;
    static std::uint64_t stale_completions(const GpuWaveNetRealtimeNode&) noexcept;
    static void observe_trace(GpuWaveNetRealtimeNode&, SharedIoTraceDrainObserver) noexcept;
    static SharedIoTraceStats trace_stats(const GpuWaveNetRealtimeNode&) noexcept;
    static SharedIoTelemetrySnapshot telemetry(const GpuWaveNetRealtimeNode&) noexcept;
    static SharedIoTraceDrainResult last_trace_drain(const GpuWaveNetRealtimeNode&) noexcept;
    static SharedIoTraceRecord last_terminal(const GpuWaveNetRealtimeNode&) noexcept;
    static std::uint64_t trace_engine(const GpuWaveNetRealtimeNode&) noexcept;
    static bool prepare(GpuWaveNetRealtimeNode&,
                        std::vector<std::unique_ptr<WaveNetRealtimeChannel>>,
                        std::uint64_t first_sequence = 0);
};
} // namespace pulp::gpu_audio::detail
