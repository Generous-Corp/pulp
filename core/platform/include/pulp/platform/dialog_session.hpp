#pragma once

/// @file dialog_session.hpp
/// Handle to an asynchronous dialog, alert or menu that is open.

#include <memory>

namespace pulp::platform {

namespace detail {
struct DialogSessionState;
} // namespace detail

/// Returned by the asynchronous dialog calls. Holding it is optional:
/// dropping the handle leaves the dialog open. Main thread only.
class DialogSession {
  public:
    DialogSession() = default;
    explicit DialogSession(std::shared_ptr<detail::DialogSessionState> state);

    /// Close the dialog as if the user dismissed it. Its completion still
    /// runs once, with the dismissed result (cancel, no, nothing chosen).
    void cancel();
    /// Whether the dialog is still showing.
    bool open() const;

  private:
    std::shared_ptr<detail::DialogSessionState> state_;
};

} // namespace pulp::platform
