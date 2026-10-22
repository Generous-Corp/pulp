#pragma once

#include "detail/shared_io_program_session.hpp"
#include "detail/wavenet_realtime_channel.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>

namespace pulp::test {

// The WaveNet node's private channel seam over a prepared shared-I/O program
// session, so tests can put a chosen provider (a fake, or a physical provider
// with an injected fault) behind the node. It mirrors GpuWaveNetSession, which
// owns its provider and cannot take either.
class ProgramSessionChannel final : public pulp::gpu_audio::detail::WaveNetRealtimeChannel {
  public:
    using Session = pulp::gpu_audio::detail::SharedIoProgramSession;

    explicit ProgramSessionChannel(std::unique_ptr<Session> session)
        : session_(std::move(session)) {}

    bool submit(std::span<const float> input,
                const pulp::gpu_audio::detail::SharedIoSubmissionTicket& ticket) noexcept override {
        const auto sequence = ticket.sequence;
        auto lease = session_->acquire_input(sequence, 0);
        if (!lease || lease->bytes.size() != input.size_bytes())
            return false;
        std::copy(input.begin(), input.end(), reinterpret_cast<float*>(lease->bytes.data()));
        const auto token = Session::SubmitToken::admit(lease->token);
        if (session_->submit(token)) {
            // The session's own ticket names its preparation epoch; the node
            // needs the ticket it admitted the block with.
            node_tickets_[sequence % node_tickets_.size()] = ticket;
            return true;
        }
        (void)session_->cancel(token);
        return false;
    }
    void service(std::uint64_t now) noexcept override {
        session_->service(now);
    }
    void service_until(std::uint64_t now, std::uint64_t deadline) noexcept override {
        session_->service_until(now, deadline);
    }
    std::optional<pulp::gpu_audio::detail::WaveNetChannelCompletion>
    receive(std::span<float> output) noexcept override {
        auto result = receive_result(output);
        if (!result)
            return std::nullopt;
        pulp::gpu_audio::detail::WaveNetChannelCompletion completion{*result, {}};
        auto& entry = node_tickets_[result->sequence % node_tickets_.size()];
        if (entry.accepted() && entry.sequence == result->sequence) {
            completion.ticket = entry;
            entry = {};
        }
        return completion;
    }
    std::optional<pulp::gpu_audio::GpuWaveNetBlockResult>
    receive_result(std::span<float> output) noexcept {
        using pulp::gpu_audio::GpuWaveNetBlockStatus;
        const auto completion = session_->pop_completion();
        if (!completion)
            return std::nullopt;
        pulp::gpu_audio::GpuWaveNetBlockResult result{
            .sequence = completion->token.slot.stream_sequence,
            .status = GpuWaveNetBlockStatus::ProviderFailed,
            .late = completion->late};
        if (completion->status !=
            pulp::gpu_audio::detail::SharedIoArena::CompletionStatus::RetiredSuccess) {
            (void)session_->discard_completion(*completion);
            return result;
        }
        auto lease = session_->acquire_output(*completion);
        if (!lease || lease->bytes.size() < output.size_bytes()) {
            if (lease)
                (void)session_->release_output({lease->token});
            return result;
        }
        std::copy_n(reinterpret_cast<const float*>(lease->bytes.data()), output.size(),
                    output.begin());
        if (session_->release_output({lease->token}))
            result.status = GpuWaveNetBlockStatus::GpuDelivered;
        return result;
    }
    bool release() noexcept override {
        return session_->release();
    }
    bool lost() const noexcept override {
        const auto* provider = session_->owned_provider();
        return provider != nullptr && provider->device_lost();
    }

  private:
    std::unique_ptr<Session> session_;
    std::array<pulp::gpu_audio::detail::SharedIoSubmissionTicket, 64> node_tickets_{};
};

} // namespace pulp::test
