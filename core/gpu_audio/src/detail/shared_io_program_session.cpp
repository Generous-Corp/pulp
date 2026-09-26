#include "shared_io_program_session.hpp"

namespace pulp::gpu_audio::detail {

SharedIoProgramSession::~SharedIoProgramSession() {
    (void)release();
}

bool SharedIoProgramSession::prepare(ProviderPair pair, Config config) {
    if (prepared_ || provider_ || plan_.prepared() || !pair.provider || !pair.program ||
        config.slots == 0 || config.input_bytes_per_slot == 0 || config.output_bytes_per_slot == 0)
        return false;

    // Retain the provider even when the plan's transaction fails: the arena
    // may have allocated/imported slots and requires an explicit release
    // barrier before ownership can be discarded.
    provider_ = std::move(pair.provider);
    if (!plan_.prepare(*provider_, config, std::move(pair.program)))
        return false;
    prepared_ = true;
    return true;
}

std::optional<SharedIoProgramSession::InputLease>
SharedIoProgramSession::acquire_input(std::uint64_t sequence, std::uint64_t deadline_ns) noexcept {
    if (!prepared_)
        return std::nullopt;
    return plan_.acquire_input(sequence, deadline_ns);
}

bool SharedIoProgramSession::submit(const SubmitToken& token) noexcept {
    return prepared_ && plan_.submit(token);
}

bool SharedIoProgramSession::cancel(const SubmitToken& token) noexcept {
    return prepared_ && plan_.cancel(token);
}

std::size_t SharedIoProgramSession::service(std::uint64_t now_ns) noexcept {
    if (!prepared_ || !provider_)
        return 0;
    return plan_.drain(now_ns);
}

std::optional<SharedIoProgramSession::Completion>
SharedIoProgramSession::pop_completion() noexcept {
    if (!prepared_)
        return std::nullopt;
    return plan_.pop_completion();
}

std::optional<SharedIoProgramSession::OutputLease>
SharedIoProgramSession::acquire_output(const Completion& completion) noexcept {
    if (!prepared_)
        return std::nullopt;
    return plan_.acquire_output(completion);
}

bool SharedIoProgramSession::release_output(const SharedIoArena::ReleaseRecord& record) noexcept {
    return prepared_ && plan_.release_output(record);
}

bool SharedIoProgramSession::expire_delivery(const Completion& completion) noexcept {
    return prepared_ && plan_.expire_delivery(completion);
}

bool SharedIoProgramSession::discard_completion(const Completion& completion) noexcept {
    return prepared_ && plan_.discard_completion(completion);
}

bool SharedIoProgramSession::reprime_when_quiescent() noexcept {
    if (!prepared_ || !provider_ || !provider_->can_resume_after_drain())
        return false;
    return plan_.reprime_when_quiescent();
}

bool SharedIoProgramSession::release() noexcept {
    if (!provider_ && !plan_.prepared()) {
        prepared_ = false;
        return true;
    }
    if (!plan_.release())
        return false;
    prepared_ = false;
    provider_.reset();
    return true;
}

} // namespace pulp::gpu_audio::detail
