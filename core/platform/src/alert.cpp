#include <pulp/platform/alert.hpp>

#include "dialog_internal.hpp"

#include <mutex>
#include <utility>

namespace pulp::platform {

namespace {

std::mutex g_presenter_mutex;
AlertPresenter g_presenter;

AlertPresenter current_presenter() {
    std::lock_guard<std::mutex> lock(g_presenter_mutex);
    return g_presenter;
}

} // namespace

const char* to_string(AlertResult result) noexcept {
    switch (result) {
    case AlertResult::ok:
        return "ok";
    case AlertResult::cancel:
        return "cancel";
    case AlertResult::yes:
        return "yes";
    case AlertResult::no:
        return "no";
    case AlertResult::unavailable:
        return "unavailable";
    }
    return "unknown";
}

AlertButtonPlan alert_button_plan(const AlertRequest& request) {
    AlertButtonPlan plan;
    switch (request.buttons) {
    case AlertButtons::ok:
        plan.titles = {"OK"};
        plan.results = {AlertResult::ok};
        plan.escape_index = 0;
        break;
    case AlertButtons::ok_cancel:
        plan.titles = {"OK", "Cancel"};
        plan.results = {AlertResult::ok, AlertResult::cancel};
        plan.escape_index = 1;
        break;
    case AlertButtons::yes_no:
        plan.titles = {"Yes", "No"};
        plan.results = {AlertResult::yes, AlertResult::no};
        plan.escape_index = 1;
        break;
    }
    if (request.button_labels.size() == plan.titles.size())
        plan.titles = request.button_labels;
    return plan;
}

AlertResult alert_result_for_button(const AlertButtonPlan& plan, long index) {
    if (index >= 0 && static_cast<std::size_t>(index) < plan.results.size())
        return plan.results[static_cast<std::size_t>(index)];
    return plan.results.empty() ? AlertResult::unavailable : plan.results[plan.escape_index];
}

void set_alert_presenter(AlertPresenter presenter) {
    std::lock_guard<std::mutex> lock(g_presenter_mutex);
    g_presenter = std::move(presenter);
}

DialogSession show_alert_async(MainThreadScope& scope, NativeViewHandle parent,
                               AlertRequest request, std::function<void(AlertResult)> done) {
    auto state = std::make_shared<detail::DialogSessionState>();
    const AlertButtonPlan plan = alert_button_plan(request);
    auto finish = detail::make_finisher<AlertResult>(scope, state, std::move(done));
    auto respond = [plan, finish](long index) { finish(alert_result_for_button(plan, index)); };

    const AlertPresenter presenter = current_presenter();
    if (presenter.present) {
        presenter.present(request, plan, respond);
        auto cancel = presenter.cancel;
        const long escape = static_cast<long>(plan.escape_index);
        state->close = [cancel, respond, escape] {
            if (cancel)
                cancel();
            respond(escape);
        };
    } else {
        state->close = detail::present_native_alert(parent, request, plan, respond);
        if (!state->close) {
            finish(AlertResult::unavailable);
            return DialogSession(state);
        }
    }
    detail::bind_to_scope(scope, state);
    return DialogSession(state);
}

} // namespace pulp::platform
