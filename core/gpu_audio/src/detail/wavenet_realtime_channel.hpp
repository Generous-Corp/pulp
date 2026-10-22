#pragma once
#include "shared_io_completion_notifier.hpp"
#include "shared_io_stamped_bridge.hpp"
#include "shared_io_execution_contract.hpp"
#include "shared_io_provider_identity.hpp"
#include "shared_io_trace.hpp"
#include <pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>
#include <algorithm>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace pulp::gpu_audio::detail {
// Private access to the provider a GpuWaveNetSession prepared.
struct GpuWaveNetSessionAccess {
    static SharedIoProviderIdentity provider_identity(const GpuWaveNetSession&) noexcept;
    // Host only, while no service call runs. False when the session's provider
    // has no completion event source.
    static bool set_completion_notifier(GpuWaveNetSession&, SharedIoCompletionNotifier) noexcept;
};
// Private access to a session's provider-pool path.
struct GpuWaveNetSessionPoolAccess {
    static bool recycle(GpuWaveNetSession&, std::uint64_t budget_ns) noexcept;
    static std::string provider_key(const GpuWaveNetSession&);
    static std::string requested_provider_key();
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
    // Host or serialized worker: release within `budget_ns`. On expiry the
    // provider abandons to quarantine and this returns false, keeping
    // ownership for a later release().
    virtual bool release_bounded(std::uint64_t budget_ns) noexcept {
        (void)budget_ns;
        return release();
    }
    // Provider pool hooks: drain within `budget_ns` (zero: the provider's
    // default) and reopen for a later stream, and name the provider's kind and
    // revision. A channel that cannot be kept warm returns false / empty.
    virtual bool recycle(std::uint64_t budget_ns) noexcept {
        (void)budget_ns;
        return false;
    }
    virtual std::string provider_key() const {
        return {};
    }
    // Serialized worker diagnostic: the provider's device is gone, so nothing
    // submitted to this channel will complete successfully again.
    virtual bool lost() const noexcept {
        return false;
    }
    // Non-RT: the identity the channel's provider reports for itself.
    virtual SharedIoProviderIdentity provider_identity() const noexcept {
        return {};
    }
    // Host only, while no service call runs: route this channel's provider
    // completion events to `notifier`, or clear with {} (see
    // SharedIoArenaProvider::set_completion_notifier). False when the channel
    // has no completion event source.
    virtual bool set_completion_notifier(SharedIoCompletionNotifier) noexcept {
        return false;
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
    // A scripted session has no bounded release and is never pooled.
    bool release_bounded(std::uint64_t budget_ns) noexcept override {
        std::fill(tickets_.begin(), tickets_.end(), SharedIoSubmissionTicket{});
        if constexpr (std::is_same_v<Session, GpuWaveNetSession>)
            return session_->release_bounded(budget_ns);
        else
            return session_->release();
    }
    bool recycle(std::uint64_t budget_ns) noexcept override {
        if constexpr (std::is_same_v<Session, GpuWaveNetSession>) {
            if (!GpuWaveNetSessionPoolAccess::recycle(*session_, budget_ns))
                return false;
            // Nothing admitted before the recycle can complete into a later stream.
            std::fill(tickets_.begin(), tickets_.end(), SharedIoSubmissionTicket{});
            return true;
        } else {
            return false;
        }
    }
    std::string provider_key() const override {
        if constexpr (std::is_same_v<Session, GpuWaveNetSession>)
            return GpuWaveNetSessionPoolAccess::provider_key(*session_);
        else
            return {};
    }
    bool lost() const noexcept override {
        return session_->device_lost();
    }
    // A scripted session is not an authenticated provider.
    SharedIoProviderIdentity provider_identity() const noexcept override {
        if constexpr (std::is_same_v<Session, GpuWaveNetSession>)
            return GpuWaveNetSessionAccess::provider_identity(*session_);
        else
            return {};
    }
    // A scripted session has no completion event source.
    bool set_completion_notifier(SharedIoCompletionNotifier notifier) noexcept override {
        if constexpr (std::is_same_v<Session, GpuWaveNetSession>)
            return GpuWaveNetSessionAccess::set_completion_notifier(*session_, notifier);
        else
            return false;
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
    // Channels a re-acquire creates in place of the production sessions.
    using ChannelFactory = std::unique_ptr<WaveNetRealtimeChannel> (*)(void*) noexcept;
    static void set_channel_factory(GpuWaveNetRealtimeNode&, ChannelFactory, void*) noexcept;
    static std::size_t quarantined_channels(const GpuWaveNetRealtimeNode&) noexcept;
    static std::uint32_t warmup_blocks(const GpuWaveNetRealtimeNode&) noexcept;
    // Places a channel in the re-acquire quarantine, as if it failed to drain.
    static void quarantine_for_testing(GpuWaveNetRealtimeNode&,
                                       std::unique_ptr<WaveNetRealtimeChannel>);
};
} // namespace pulp::gpu_audio::detail
