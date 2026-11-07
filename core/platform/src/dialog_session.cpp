#include <pulp/platform/dialog_session.hpp>

#include "dialog_internal.hpp"

namespace pulp::platform {

DialogSession::DialogSession(std::shared_ptr<detail::DialogSessionState> state)
    : state_(std::move(state)) {}

void DialogSession::cancel() {
    if (!state_ || !state_->open || state_->finished)
        return;
    if (state_->close) {
        auto close = state_->close;
        close();
    }
}

bool DialogSession::open() const {
    return state_ && state_->open;
}

} // namespace pulp::platform
