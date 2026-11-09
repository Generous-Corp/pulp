#pragma once

/// @file alert.hpp
/// Asynchronous native alerts parented to an editor's window.
///
/// On macOS the alert is an NSAlert sheet on the window that holds `parent`.
/// The call returns at once; `done` runs later, exactly once, on the main
/// thread, unless the MainThreadScope is destroyed first, in which case the
/// sheet is closed and `done` is dropped. Platforms without a native backend,
/// and a parent with no window (or one already showing a sheet), report
/// AlertResult::unavailable, also asynchronously. On platforms without a
/// main-thread backend (main_thread_supported() is false) there is nothing to
/// post to, so `done` runs inline, before show_alert_async returns.

#include <pulp/platform/dialog_session.hpp>
#include <pulp/platform/main_thread.hpp>
#include <pulp/platform/native_handle.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace pulp::platform {

enum class AlertButtons { ok, ok_cancel, yes_no };
enum class AlertStyle { informational, warning, critical };
enum class AlertResult { ok, cancel, yes, no, unavailable };

/// Short stable name ("ok", "cancel", ...).
const char* to_string(AlertResult result) noexcept;

struct AlertRequest {
    std::string title;
    std::string message;
    AlertButtons buttons = AlertButtons::ok;
    AlertStyle style = AlertStyle::informational;
    /// Replaces the default button titles, in button order, when it has
    /// exactly as many entries as the button set.
    std::vector<std::string> button_labels;
};

/// How a button set is presented: titles in order (the first is the default
/// button), the result each button gives, and which button Escape presses.
struct AlertButtonPlan {
    std::vector<std::string> titles;
    std::vector<AlertResult> results;
    std::size_t escape_index = 0;
};

/// The presentation of `request`'s buttons, with its label overrides applied.
AlertButtonPlan alert_button_plan(const AlertRequest& request);

/// The result for a dialog response that names the button at `index` (0 is
/// the first button). An index outside the plan (an abort or an
/// Escape-equivalent response) gives the Escape button's result.
AlertResult alert_result_for_button(const AlertButtonPlan& plan, long index);

/// Show `request` as an alert on the window holding `parent` (an NSView* on
/// macOS). Main thread only.
DialogSession show_alert_async(MainThreadScope& scope, NativeViewHandle parent,
                               AlertRequest request, std::function<void(AlertResult)> done);

/// Replaces the native presenter (for tests and hosts that draw their own).
/// The presenter receives the plan and must call `respond` once with a
/// button index (see alert_result_for_button); `cancel` closes it with the
/// Escape button. An empty presenter restores the native one.
struct AlertPresenter {
    std::function<void(const AlertRequest& request, const AlertButtonPlan& plan,
                       std::function<void(long button_index)> respond)>
        present;
    std::function<void()> cancel;
};
void set_alert_presenter(AlertPresenter presenter);

} // namespace pulp::platform
