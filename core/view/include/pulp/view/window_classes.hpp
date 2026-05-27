#pragma once

// High-level window classes (item 6.5 macOS plan).
//
// PR #2844 landed the canonical low-level `WindowHost` contract (native chrome,
// `attach_native_child_view`, multi-window via WindowManager). Item 6.5 sits
// on top: three Pulp-native window classes that cover the three patterns a
// standalone app — or a plugin's secondary window — needs in practice:
//
//   - `DocumentWindow` — a top-level document/editor window with a menu bar,
//     a content area that hosts a View tree, and a close-confirmation hook
//     ("Save changes?") that fires before the close actually happens.
//
//   - `DialogWindow` — a modal/quasi-modal dialog with explicit OK / Cancel
//     buttons. Dispatches a `DialogResult` (`ok` / `cancel` / `closed`) to
//     a user-supplied completion handler.
//
//   - `AlertWindow` — a simple titled message + N labelled buttons + an
//     icon hint (info / warning / error / question). Convenience overloads
//     for the three most common shapes: `info()`, `confirm()`, `error()`.
//
// All three are composition wrappers around `pulp::view::WindowHost`. They do
// NOT subclass it — that keeps the test surface small and avoids leaking
// platform plumbing into the high-level API. Headless tests construct them
// without a window factory and drive the lifecycle hooks directly.

#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulp::view {

// ── DocumentWindow ─────────────────────────────────────────────────────────

class DocumentWindow {
public:
    /// Hook called when the user requests the window to close (clicked the
    /// close button, hit ⌘W, or invoked request_close()). Returning `true`
    /// allows the close to proceed; returning `false` cancels it (e.g. when
    /// the document is dirty and the user picked "Cancel" in a Save dialog).
    /// When unset, every close is allowed.
    using CloseConfirmationHandler = std::function<bool()>;

    /// A menu item in this window's menu bar. The action runs on the main
    /// thread when the user picks the item. Both `title` and `action` are
    /// optional — a `title` with no `action` is a separator, an `action`
    /// with no `title` is rejected as a configuration error.
    struct MenuItem {
        std::string title;
        std::function<void()> action;
        // Future: shortcut, enabled flag, submenu pointer. Kept minimal so
        // the API can grow additively without breaking callers.
    };

    /// A top-level menu (e.g. "File", "Edit", "View") with its items.
    struct Menu {
        std::string title;
        std::vector<MenuItem> items;
    };

    DocumentWindow(const std::string& title, View& content_root);
    ~DocumentWindow();

    DocumentWindow(const DocumentWindow&) = delete;
    DocumentWindow& operator=(const DocumentWindow&) = delete;
    DocumentWindow(DocumentWindow&&) = delete;
    DocumentWindow& operator=(DocumentWindow&&) = delete;

    /// Set or replace the menu bar. Pulp standalone macOS apps surface this
    /// as the real NSMenu; on platforms without a global menu bar (Windows /
    /// Linux), the WindowHost implementation may surface it inline or ignore
    /// it. Stored on the class either way so the test surface is the same.
    void set_menus(std::vector<Menu> menus);
    const std::vector<Menu>& menus() const { return menus_; }

    /// Install a close-confirmation hook. Useful for "Save changes before
    /// closing?" workflows. Returns the previously-installed handler so
    /// callers can chain.
    CloseConfirmationHandler set_close_confirmation_handler(CloseConfirmationHandler handler);

    /// Programmatic close request — runs the close-confirmation handler and
    /// (if it returns true) tears down the underlying WindowHost. Returns
    /// true if the close was actually performed.
    bool request_close();

    /// The content View attached to this window's content area.
    View& content() { return *content_root_; }
    const View& content() const { return *content_root_; }

    /// The window's title.
    const std::string& title() const { return title_; }

    /// Underlying WindowHost (nullptr until show() is called or until a
    /// factory becomes available). Tests drive the lifecycle hooks via the
    /// DocumentWindow API directly and don't depend on this being non-null.
    WindowHost* host() { return host_.get(); }

    /// Create the native window and show it. Returns false if no
    /// WindowHost::Factory is registered on the current platform (the
    /// DocumentWindow stays usable as a logical container — its menus +
    /// content + close-confirm handler can still be inspected by tests).
    bool show();

    /// Hide the window without closing it (close-confirm is NOT consulted).
    void hide();

    /// True if `show()` produced a live native window.
    bool has_native_window() const { return host_ != nullptr; }

private:
    std::string title_;
    View* content_root_ = nullptr;
    std::vector<Menu> menus_;
    CloseConfirmationHandler close_handler_;
    std::unique_ptr<WindowHost> host_;
};

// ── DialogWindow ───────────────────────────────────────────────────────────

enum class DialogResult {
    ok,
    cancel,
    closed,  ///< User dismissed the dialog without explicitly choosing.
};

class DialogWindow {
public:
    using CompletionHandler = std::function<void(DialogResult)>;

    DialogWindow(const std::string& title, View& content_root);
    ~DialogWindow();

    DialogWindow(const DialogWindow&) = delete;
    DialogWindow& operator=(const DialogWindow&) = delete;

    /// Customize the OK button label (default "OK"). Empty string hides
    /// the OK button entirely.
    void set_ok_label(std::string label) { ok_label_ = std::move(label); }
    const std::string& ok_label() const { return ok_label_; }

    /// Customize the Cancel button label (default "Cancel"). Empty string
    /// hides the Cancel button — useful for AlertWindow-style "OK only"
    /// dialogs.
    void set_cancel_label(std::string label) { cancel_label_ = std::move(label); }
    const std::string& cancel_label() const { return cancel_label_; }

    /// Set the completion handler. Called exactly once when the dialog is
    /// dismissed.
    void set_completion_handler(CompletionHandler handler) {
        completion_ = std::move(handler);
    }

    /// Show the dialog. Returns false if no WindowHost::Factory exists.
    /// `modal` is a hint — on platforms that support modal native windows
    /// the host puts it into modal mode; otherwise the dialog floats over
    /// its parent.
    bool show(bool modal = true);

    /// Programmatic dismissal — call from a button handler. Fires the
    /// completion handler with the supplied result. No-op if the dialog has
    /// already been dismissed.
    void dismiss(DialogResult result);

    bool is_dismissed() const { return dismissed_; }
    DialogResult last_result() const { return last_result_; }
    bool is_modal() const { return modal_; }
    const std::string& title() const { return title_; }

    View& content() { return *content_root_; }
    WindowHost* host() { return host_.get(); }

private:
    std::string title_;
    View* content_root_ = nullptr;
    std::string ok_label_ = "OK";
    std::string cancel_label_ = "Cancel";
    CompletionHandler completion_;
    std::unique_ptr<WindowHost> host_;
    bool dismissed_ = false;
    bool modal_ = false;
    DialogResult last_result_ = DialogResult::closed;
};

// ── AlertWindow ────────────────────────────────────────────────────────────

enum class AlertIcon {
    none,
    info,
    warning,
    error,
    question,
};

class AlertWindow {
public:
    using ButtonHandler = std::function<void(std::size_t /*index*/)>;

    AlertWindow(const std::string& title, const std::string& message,
                AlertIcon icon = AlertIcon::info);
    ~AlertWindow();

    AlertWindow(const AlertWindow&) = delete;
    AlertWindow& operator=(const AlertWindow&) = delete;

    /// Append a button. Returns the index assigned to this button (0-based).
    std::size_t add_button(std::string label);

    /// Install a handler invoked with the index of the chosen button.
    /// Fires at most once.
    void set_button_handler(ButtonHandler handler) {
        button_handler_ = std::move(handler);
    }

    /// Show the alert. Returns false if no WindowHost::Factory exists.
    bool show();

    /// Programmatic button click — fires the handler with `index` and tears
    /// the alert down.
    void click_button(std::size_t index);

    bool is_dismissed() const { return dismissed_; }
    std::size_t button_count() const { return buttons_.size(); }
    const std::string& button_label(std::size_t i) const { return buttons_.at(i); }
    AlertIcon icon() const { return icon_; }
    const std::string& title() const { return title_; }
    const std::string& message() const { return message_; }
    WindowHost* host() { return host_.get(); }

    // ── Convenience factories ────────────────────────────────────────────

    /// "Info" alert with a single OK button. The handler fires with
    /// index = 0 when the user dismisses it.
    static std::unique_ptr<AlertWindow> info(const std::string& title,
                                             const std::string& message,
                                             ButtonHandler handler = {});

    /// "Question" alert with Cancel (0) and OK (1) buttons.
    static std::unique_ptr<AlertWindow> confirm(const std::string& title,
                                                const std::string& message,
                                                ButtonHandler handler = {});

    /// "Error" alert with a single OK button.
    static std::unique_ptr<AlertWindow> error(const std::string& title,
                                              const std::string& message,
                                              ButtonHandler handler = {});

    /// "Warning" alert with a single OK button — sibling of `info` /
    /// `error` (closes the gap-doc Phase 3 row
    /// "TooltipWindow + BubbleMessageComponent + AlertWindow styled"
    /// for the warning variant).
    static std::unique_ptr<AlertWindow> warning(const std::string& title,
                                                const std::string& message,
                                                ButtonHandler handler = {});

    // ── Styled-variant theme tokens ───────────────────────────────────

    /// Theme color-token names the host should look up when painting
    /// each `AlertIcon`. These names are stable identifiers — themes
    /// that define them get the styled treatment; themes that do not
    /// fall back to the default text color. Token names follow the
    /// `alert.<icon>.<role>` convention.
    struct StyleTokens {
        const char* icon_color = "";       ///< Token for the icon glyph color.
        const char* accent_color = "";     ///< Token for the chrome accent (border / title).
        const char* glyph = "";            ///< Glyph hint (e.g. "i", "!", "x", "?").
    };

    /// Returns the style tokens for an icon. The host can use these to
    /// paint the alert chrome in a theme-driven color without baking
    /// a specific palette into the alert window itself.
    ///
    /// info → "alert.info.icon" + "alert.info.accent" + glyph "i"
    /// warning → "alert.warning.icon" + "alert.warning.accent" + glyph "!"
    /// error → "alert.error.icon" + "alert.error.accent" + glyph "x"
    /// question → "alert.question.icon" + "alert.question.accent" + "?"
    /// none → empty tokens.
    static StyleTokens style_tokens_for(AlertIcon icon);

    /// Convenience that returns this alert's styled tokens.
    StyleTokens style_tokens() const { return style_tokens_for(icon_); }

private:
    std::string title_;
    std::string message_;
    AlertIcon icon_;
    std::vector<std::string> buttons_;
    ButtonHandler button_handler_;
    std::unique_ptr<View> root_view_;
    std::unique_ptr<WindowHost> host_;
    bool dismissed_ = false;
};

} // namespace pulp::view
