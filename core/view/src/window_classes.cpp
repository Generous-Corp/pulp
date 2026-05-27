#include <pulp/view/window_classes.hpp>

#include <utility>

namespace pulp::view {

// ── DocumentWindow ─────────────────────────────────────────────────────────

DocumentWindow::DocumentWindow(const std::string& title, View& content_root)
    : title_(title), content_root_(&content_root) {}

DocumentWindow::~DocumentWindow() = default;

void DocumentWindow::set_menus(std::vector<Menu> menus) {
    menus_ = std::move(menus);
}

DocumentWindow::CloseConfirmationHandler
DocumentWindow::set_close_confirmation_handler(CloseConfirmationHandler handler) {
    auto prev = std::move(close_handler_);
    close_handler_ = std::move(handler);
    return prev;
}

bool DocumentWindow::request_close() {
    // Run the close-confirmation hook on the caller's thread. If it returns
    // false, abort the close (e.g. the user picked "Cancel" in the Save
    // dialog the hook presented).
    if (close_handler_ && !close_handler_()) {
        return false;
    }
    if (host_) {
        host_->request_close();
    }
    return true;
}

bool DocumentWindow::show() {
    if (host_) {
        host_->show();
        return true;
    }
    WindowOptions opts;
    opts.title = title_;
    opts.width = 800;
    opts.height = 600;
    host_ = WindowHost::create(*content_root_, opts);
    if (!host_) return false;
    // Route native close affordances (title-bar X, Cmd+W, Alt+F4) through
    // request_close() so the close-confirmation handler ALWAYS runs. Without
    // this wiring, the title-bar X would bypass close_handler_ and let
    // unsaved-changes veto logic be silently overridden (Codex PR #3006).
    host_->set_close_callback([this]() {
        (void)this->request_close();
    });
    host_->show();
    return true;
}

void DocumentWindow::hide() {
    if (host_) host_->hide();
}

// ── DialogWindow ───────────────────────────────────────────────────────────

DialogWindow::DialogWindow(const std::string& title, View& content_root)
    : title_(title), content_root_(&content_root) {}

DialogWindow::~DialogWindow() = default;

bool DialogWindow::show(bool modal) {
    modal_ = modal;
    if (host_) {
        host_->show();
        return true;
    }
    WindowOptions opts;
    opts.title = title_;
    opts.width = 480;
    opts.height = 240;
    // DialogWindow defaults to fixed-size — typical dialog convention.
    opts.resizable = false;
    host_ = WindowHost::create(*content_root_, opts);
    if (!host_) return false;
    // Wire the native close affordance to dismiss(closed) so callers waiting
    // on the completion handler always receive a result — closing via the
    // title-bar X / Cmd+W must not leave a modal dance pending forever
    // (Codex PR #3006).
    host_->set_close_callback([this]() {
        this->dismiss(DialogResult::closed);
    });
    host_->show();
    return true;
}

void DialogWindow::dismiss(DialogResult result) {
    if (dismissed_) return;
    dismissed_ = true;
    last_result_ = result;
    if (host_) {
        host_->request_close();
    }
    if (completion_) {
        // Move the completion handler aside before invoking so a handler
        // that itself destroys the DialogWindow doesn't free us mid-call.
        auto h = std::move(completion_);
        completion_ = {};
        h(result);
    }
}

// ── AlertWindow ────────────────────────────────────────────────────────────

AlertWindow::AlertWindow(const std::string& title, const std::string& message,
                         AlertIcon icon)
    : title_(title), message_(message), icon_(icon) {}

AlertWindow::~AlertWindow() = default;

std::size_t AlertWindow::add_button(std::string label) {
    buttons_.push_back(std::move(label));
    return buttons_.size() - 1;
}

bool AlertWindow::show() {
    if (buttons_.empty()) {
        // Force at least an OK button so the user always has a way out.
        buttons_.emplace_back("OK");
    }
    if (host_) {
        host_->show();
        return true;
    }
    // AlertWindow owns its own root View — the platform host paints the
    // alert chrome (icon + message + buttons) over it.
    if (!root_view_) root_view_ = std::make_unique<View>();
    WindowOptions opts;
    opts.title = title_;
    opts.width = 400;
    opts.height = 160;
    opts.resizable = false;
    host_ = WindowHost::create(*root_view_, opts);
    if (!host_) return false;
    host_->show();
    return true;
}

void AlertWindow::click_button(std::size_t index) {
    if (dismissed_) return;
    dismissed_ = true;
    if (host_) {
        host_->request_close();
    }
    if (button_handler_) {
        auto h = std::move(button_handler_);
        button_handler_ = {};
        h(index);
    }
}

std::unique_ptr<AlertWindow> AlertWindow::info(const std::string& title,
                                                const std::string& message,
                                                ButtonHandler handler) {
    auto a = std::make_unique<AlertWindow>(title, message, AlertIcon::info);
    a->add_button("OK");
    if (handler) a->set_button_handler(std::move(handler));
    return a;
}

std::unique_ptr<AlertWindow> AlertWindow::confirm(const std::string& title,
                                                   const std::string& message,
                                                   ButtonHandler handler) {
    auto a = std::make_unique<AlertWindow>(title, message, AlertIcon::question);
    a->add_button("Cancel");
    a->add_button("OK");
    if (handler) a->set_button_handler(std::move(handler));
    return a;
}

std::unique_ptr<AlertWindow> AlertWindow::error(const std::string& title,
                                                 const std::string& message,
                                                 ButtonHandler handler) {
    auto a = std::make_unique<AlertWindow>(title, message, AlertIcon::error);
    a->add_button("OK");
    if (handler) a->set_button_handler(std::move(handler));
    return a;
}

} // namespace pulp::view
