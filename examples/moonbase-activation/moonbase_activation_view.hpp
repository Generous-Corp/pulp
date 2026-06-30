// Pulp-native activation UI, rendered entirely in the Pulp view tree (no WebView
// / OS overlay) so it composites through the Skia/canvas path and is capturable
// by Pulp's headless tooling. Two surfaces share one layout:
//
//   - build_activation_view(): a STATIC panel for a const controller — used by
//     tests and the headless screenshot tool (one panel per preview screen).
//   - ActivationPanel: an INTERACTIVE View whose action button is wired to the
//     controller (activate / cancel / deactivate) and which rebuild()s itself
//     when the screen changes — used by the live plugin/standalone editor.
//
// Every color resolves through pulp::view::Theme tokens (Ink & Signal naming):
// there is NO hard-coded Moonbase palette. The panel inherits the host theme and
// is reskinnable via Theme / theme.json / token import. Moonbase branding lives
// in the hosted portal/browser flow, not here.

#pragma once

#include "moonbase_activation_controller.hpp"

#include <pulp/view/buttons.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <chrono>
#include <ctime>
#include <functional>
#include <memory>
#include <string>

namespace moonbase_pulp {

namespace detail {

// Root that paints the themed background and lays its children out by explicit
// bounds (suppressing the flex pass) for a deterministic, capturable panel.
class ActivationPanelRoot : public pulp::view::View {
public:
    void paint(pulp::canvas::Canvas& canvas) override
    {
        const auto bg = resolve_color("bg.primary", pulp::canvas::Color::rgba8(30, 30, 46));
        canvas.set_fill_color(bg);
        canvas.fill_rect(0, 0, bounds().width, bounds().height);
    }
    void layout_children() override {}
};

inline std::string format_date(std::chrono::system_clock::time_point tp)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

inline std::string title_for(MoonbaseActivationController::Screen screen)
{
    using S = MoonbaseActivationController::Screen;
    switch (screen) {
        case S::Loading:     return "Checking your license…";
        case S::Welcome:     return "Activate to unlock";
        case S::BrowserWait: return "Waiting for activation…";
        case S::Success:     return "You're all set";
        case S::Trial:       return "Trial active";
        case S::Details:     return "Licensed";
        case S::Expired:     return "Trial expired";
        case S::Error:       return "Something went wrong";
    }
    return "";
}

// A multi-line license summary mirroring the fields a real activation panel
// shows: who it's licensed to, the product, the activation type, expiry, and
// seat usage — read straight from the validated moonbase::license.
inline std::string details_for(const MoonbaseActivationController& controller)
{
    const auto& maybe = controller.license();
    if (!maybe) return "This device is activated.";
    const moonbase::license& lic = *maybe;

    std::string s = "Licensed to: " + lic.issued_to.name;
    if (!lic.issued_to.email.empty()) s += " (" + lic.issued_to.email + ")";
    s += "\n";
    if (!lic.licensed_product.name.empty())
        s += "Product: " + lic.licensed_product.name + "\n";
    s += "Type: ";
    s += lic.trial ? "Trial" : moonbase::to_string(lic.method);
    if (lic.subscription_id) s += " · Subscription";
    s += "\n";
    s += "Expires: ";
    s += lic.expires_at ? format_date(*lic.expires_at) : std::string("Never");
    if (lic.seat_count) {
        s += "\nSeats: ";
        if (lic.seats_used) s += std::to_string(*lic.seats_used) + " / ";
        s += std::to_string(*lic.seat_count);
    }
    return s;
}

inline std::string body_for(const MoonbaseActivationController& controller)
{
    using S = MoonbaseActivationController::Screen;
    switch (controller.screen()) {
        case S::Loading:     return "Validating your stored license.";
        case S::Welcome:     return "Activate this device to unlock the full version.";
        case S::BrowserWait: return "Finish activating in the browser window we opened.";
        case S::Success:     return "This device is now activated.";
        case S::Trial:       return details_for(controller);
        case S::Details:     return details_for(controller);
        case S::Expired:     return "Your trial has ended. Activate to continue.";
        case S::Error:       return controller.status_message().empty()
                                 ? "Please try again."
                                 : controller.status_message();
    }
    return "";
}

inline std::string action_label_for(MoonbaseActivationController::Screen screen)
{
    using S = MoonbaseActivationController::Screen;
    switch (screen) {
        case S::Welcome:
        case S::Expired:
        case S::Error:       return "Activate";
        case S::Trial:       return "Unlock full version";
        case S::BrowserWait: return "Cancel";
        case S::Success:
        case S::Details:     return "Deactivate this device";
        case S::Loading:     return "";
    }
    return "";
}

// The action on the current screen is "destructive/neutral" (Deactivate / Cancel)
// rather than the primary call-to-action.
inline bool action_is_secondary(MoonbaseActivationController::Screen screen)
{
    using S = MoonbaseActivationController::Screen;
    return screen == S::Details || screen == S::Success || screen == S::BrowserWait;
}

// Lay the title / body / action button into `root` for the controller's current
// screen. `on_action` (when set) is wired to the action button's click; an empty
// handler yields a static, non-interactive panel (tests + screenshots).
inline void lay_out_panel(pulp::view::View& root,
                          const MoonbaseActivationController& controller,
                          const pulp::view::Theme& theme,
                          float width, float height,
                          const std::function<void()>& on_action)
{
    using pulp::view::Label;
    using pulp::view::TextButton;
    using Color = pulp::canvas::Color;

    const auto text_primary = theme.color("text.primary").value_or(Color::rgba8(205, 214, 244));
    const auto text_secondary = theme.color("text.secondary").value_or(Color::rgba8(166, 173, 200));

    const float margin = 32.0f;
    const float content_w = width - 2 * margin;

    auto title = std::make_unique<Label>(title_for(controller.screen()));
    title->set_font_size(24.0f);
    title->set_font_weight(700);
    title->set_text_color(text_primary);
    title->set_bounds({margin, 36.0f, content_w, 32.0f});
    root.add_child(std::move(title));

    auto body = std::make_unique<Label>(body_for(controller));
    body->set_font_size(15.0f);
    body->set_multi_line(true);
    body->set_text_color(text_secondary);
    body->set_bounds({margin, 80.0f, content_w, height - 80.0f - 64.0f});
    root.add_child(std::move(body));

    const auto action = action_label_for(controller.screen());
    if (!action.empty()) {
        auto button = std::make_unique<TextButton>(action);
        button->set_style(action_is_secondary(controller.screen())
                              ? TextButton::Style::secondary
                              : TextButton::Style::primary);
        button->set_bounds({margin, height - 40.0f - 24.0f, 220.0f, 40.0f});
        if (on_action) button->on_click = on_action;
        root.add_child(std::move(button));
    }
}

} // namespace detail

// Build a STATIC activation panel for the controller's current screen, themed via
// `theme`. The action button is drawn but unwired — for tests and headless
// screenshots. For the live editor use ActivationPanel.
inline std::unique_ptr<pulp::view::View> build_activation_view(
    const MoonbaseActivationController& controller,
    const pulp::view::Theme& theme,
    float width = 420.0f,
    float height = 280.0f)
{
    auto root = std::make_unique<detail::ActivationPanelRoot>();
    root->set_theme(theme);
    root->set_bounds({0, 0, width, height});
    detail::lay_out_panel(*root, controller, theme, width, height, /*on_action=*/{});
    return root;
}

// Interactive activation panel for the live plugin/standalone editor. Its action
// button drives the controller (activate / cancel / deactivate); after any action
// — or whenever the controller's screen changes underneath it (async start, a
// fulfilled poll) — call refresh_if_changed() from the editor's frame tick to
// rebuild and repaint.
class ActivationPanel : public detail::ActivationPanelRoot {
public:
    ActivationPanel(MoonbaseActivationController& controller,
                    pulp::view::Theme theme,
                    float width = 420.0f,
                    float height = 280.0f)
        : controller_(controller), theme_(std::move(theme)),
          width_(width), height_(height)
    {
        set_theme(theme_);
        set_bounds({0, 0, width_, height_});
        rebuild();
    }

    // Rebuild children for the controller's current screen, wiring the action.
    void rebuild()
    {
        while (child_count() > 0) {
            if (auto* c = child_at(0)) remove_child(c); else break;
        }
        displayed_ = controller_.screen();
        detail::lay_out_panel(*this, controller_, theme_, width_, height_,
                              [this]() { perform_action(); });
    }

    // If the controller advanced to a different screen since the last rebuild,
    // rebuild + repaint. Returns true if it rebuilt. UI thread.
    bool refresh_if_changed()
    {
        if (controller_.screen() == displayed_) return false;
        rebuild();
        request_repaint();
        return true;
    }

    MoonbaseActivationController::Screen displayed_screen() const { return displayed_; }

private:
    void perform_action()
    {
        using S = MoonbaseActivationController::Screen;
        switch (controller_.screen()) {
            case S::Welcome:
            case S::Trial:
            case S::Expired:
            case S::Error:       controller_.begin_online_activation(); break;
            case S::BrowserWait: controller_.cancel_activation(); break;
            case S::Success:
            case S::Details:     controller_.deactivate(); break;
            case S::Loading:     break;
        }
        // Do NOT rebuild here: this runs inside the action button's own click
        // dispatch, and rebuild() would destroy that button mid-event (a
        // use-after-free). The action changed the controller's screen, so the
        // editor's next frame tick — refresh_if_changed() — rebuilds safely once
        // the event has unwound. Just ask for a repaint.
        request_repaint();
    }

    MoonbaseActivationController& controller_;
    pulp::view::Theme theme_;
    float width_;
    float height_;
    MoonbaseActivationController::Screen displayed_ =
        MoonbaseActivationController::Screen::Loading;
};

} // namespace moonbase_pulp
