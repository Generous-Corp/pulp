// High-level window classes (item 6.5 macOS plan):
//   - DocumentWindow — menu bar + content view + close-confirmation hook.
//   - DialogWindow   — modal/quasi-modal with OK/Cancel + DialogResult.
//   - AlertWindow    — titled message + N buttons + icon, with info/
//                       confirm/error convenience factories.
//
// These tests run headless. WindowHost::create returns nullptr without a
// registered Factory on non-Apple platforms; on macOS it returns a real
// NSWindow-backed host. The headless surface tested here is the high-level
// composition — menu storage, close-confirmation routing, dismiss/result,
// button handlers — which is exercised without needing the platform host.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/view.hpp>
#include <pulp/view/window_classes.hpp>
#include <pulp/view/window_host.hpp>

#include <optional>

using namespace pulp::view;

namespace {

// RAII helper that uninstalls any WindowHost::Factory the host platform
// might have registered, so the tests always run in the "no factory" branch
// of `show()` (i.e. the headless composition path).
class ScopedNoWindowFactory {
public:
    ScopedNoWindowFactory() { WindowHost::clear_factory(); }
    ~ScopedNoWindowFactory() { WindowHost::clear_factory(); }
};

} // namespace

// ── DocumentWindow ─────────────────────────────────────────────────────────

TEST_CASE("DocumentWindow stores menus and content reference",
          "[view][window-classes][document-window][item-6-5]") {
    ScopedNoWindowFactory no_factory;
    View root;
    DocumentWindow doc("Untitled", root);

    REQUIRE(doc.title() == "Untitled");
    REQUIRE(&doc.content() == &root);
    REQUIRE(doc.menus().empty());

    std::vector<DocumentWindow::Menu> menus = {
        {"File", {{"New",  [] {}}, {"Open", [] {}}, {"Save", [] {}}}},
        {"Edit", {{"Undo", [] {}}, {"Redo", [] {}}}},
    };
    doc.set_menus(menus);
    REQUIRE(doc.menus().size() == 2);
    REQUIRE(doc.menus()[0].title == "File");
    REQUIRE(doc.menus()[0].items.size() == 3);
    REQUIRE(doc.menus()[1].title == "Edit");
}

TEST_CASE("DocumentWindow close confirmation handler can veto",
          "[view][window-classes][document-window][item-6-5]") {
    ScopedNoWindowFactory no_factory;
    View root;
    DocumentWindow doc("Doc", root);

    int hook_calls = 0;
    bool allow_close = false;
    doc.set_close_confirmation_handler([&] {
        ++hook_calls;
        return allow_close;
    });

    REQUIRE_FALSE(doc.request_close());
    REQUIRE(hook_calls == 1);

    allow_close = true;
    REQUIRE(doc.request_close());
    REQUIRE(hook_calls == 2);
}

TEST_CASE("DocumentWindow has no native window before show",
          "[view][window-classes][document-window][item-6-5]") {
    ScopedNoWindowFactory no_factory;
    View root;
    DocumentWindow doc("Doc", root);
    REQUIRE_FALSE(doc.has_native_window());
    // We deliberately don't call show() here — on macOS the built-in
    // NSWindow factory is always available, so show() *will* create a
    // native window even with no registered cross-platform factory.
    // On non-Apple platforms show() would return false. Either way, the
    // DocumentWindow stays usable as a logical container before show().
    REQUIRE(doc.title() == "Doc");
    REQUIRE(&doc.content() == &root);
}

TEST_CASE("DocumentWindow set_close_confirmation_handler returns the previous handler",
          "[view][window-classes][document-window][item-6-5]") {
    ScopedNoWindowFactory no_factory;
    View root;
    DocumentWindow doc("Doc", root);

    auto first_calls = std::make_shared<int>(0);
    doc.set_close_confirmation_handler([first_calls] {
        ++*first_calls;
        return true;
    });

    auto returned = doc.set_close_confirmation_handler([] { return true; });
    REQUIRE(returned);
    returned();
    REQUIRE(*first_calls == 1);
}

// ── DialogWindow ───────────────────────────────────────────────────────────

TEST_CASE("DialogWindow defaults match dialog convention",
          "[view][window-classes][dialog-window][item-6-5]") {
    ScopedNoWindowFactory no_factory;
    View content;
    DialogWindow dlg("Pick one", content);
    REQUIRE(dlg.title() == "Pick one");
    REQUIRE(dlg.ok_label() == "OK");
    REQUIRE(dlg.cancel_label() == "Cancel");
    REQUIRE_FALSE(dlg.is_dismissed());
}

TEST_CASE("DialogWindow dismiss fires the completion handler with the right result",
          "[view][window-classes][dialog-window][item-6-5]") {
    ScopedNoWindowFactory no_factory;
    View content;
    DialogWindow dlg("Dlg", content);

    std::optional<DialogResult> received;
    dlg.set_completion_handler([&](DialogResult r) { received = r; });

    dlg.dismiss(DialogResult::ok);
    REQUIRE(dlg.is_dismissed());
    REQUIRE(dlg.last_result() == DialogResult::ok);
    REQUIRE(received == DialogResult::ok);
}

TEST_CASE("DialogWindow dismiss is idempotent",
          "[view][window-classes][dialog-window][item-6-5]") {
    ScopedNoWindowFactory no_factory;
    View content;
    DialogWindow dlg("Dlg", content);
    int calls = 0;
    dlg.set_completion_handler([&](DialogResult) { ++calls; });
    dlg.dismiss(DialogResult::cancel);
    dlg.dismiss(DialogResult::ok);  // ignored
    REQUIRE(calls == 1);
    REQUIRE(dlg.last_result() == DialogResult::cancel);
}

TEST_CASE("DialogWindow customizable button labels",
          "[view][window-classes][dialog-window][item-6-5]") {
    ScopedNoWindowFactory no_factory;
    View content;
    DialogWindow dlg("Dlg", content);
    dlg.set_ok_label("Apply");
    dlg.set_cancel_label("Discard");
    REQUIRE(dlg.ok_label() == "Apply");
    REQUIRE(dlg.cancel_label() == "Discard");
}

// ── AlertWindow ────────────────────────────────────────────────────────────

TEST_CASE("AlertWindow basics",
          "[view][window-classes][alert-window][item-6-5]") {
    ScopedNoWindowFactory no_factory;
    AlertWindow a("Heads up", "Something happened.", AlertIcon::warning);
    REQUIRE(a.title() == "Heads up");
    REQUIRE(a.message() == "Something happened.");
    REQUIRE(a.icon() == AlertIcon::warning);
    REQUIRE(a.button_count() == 0);

    REQUIRE(a.add_button("OK") == 0);
    REQUIRE(a.add_button("Cancel") == 1);
    REQUIRE(a.button_count() == 2);
    REQUIRE(a.button_label(0) == "OK");
    REQUIRE(a.button_label(1) == "Cancel");
}

TEST_CASE("AlertWindow click_button fires handler and dismisses",
          "[view][window-classes][alert-window][item-6-5]") {
    ScopedNoWindowFactory no_factory;
    AlertWindow a("T", "M");
    a.add_button("A");
    a.add_button("B");
    a.add_button("C");

    std::optional<std::size_t> chosen;
    a.set_button_handler([&](std::size_t i) { chosen = i; });

    a.click_button(1);
    REQUIRE(chosen == 1);
    REQUIRE(a.is_dismissed());

    // Subsequent clicks are no-ops.
    a.click_button(0);
    REQUIRE(chosen == 1);
}

TEST_CASE("AlertWindow info() factory has a single OK button",
          "[view][window-classes][alert-window][item-6-5]") {
    ScopedNoWindowFactory no_factory;
    int handler_calls = 0;
    auto a = AlertWindow::info("Title", "Message",
                               [&](std::size_t) { ++handler_calls; });
    REQUIRE(a);
    REQUIRE(a->icon() == AlertIcon::info);
    REQUIRE(a->button_count() == 1);
    REQUIRE(a->button_label(0) == "OK");
    a->click_button(0);
    REQUIRE(handler_calls == 1);
}

TEST_CASE("AlertWindow confirm() factory has Cancel + OK",
          "[view][window-classes][alert-window][item-6-5]") {
    ScopedNoWindowFactory no_factory;
    auto a = AlertWindow::confirm("Title", "Are you sure?");
    REQUIRE(a);
    REQUIRE(a->icon() == AlertIcon::question);
    REQUIRE(a->button_count() == 2);
    REQUIRE(a->button_label(0) == "Cancel");
    REQUIRE(a->button_label(1) == "OK");
}

TEST_CASE("AlertWindow error() factory uses the error icon",
          "[view][window-classes][alert-window][item-6-5]") {
    ScopedNoWindowFactory no_factory;
    auto a = AlertWindow::error("Title", "Boom");
    REQUIRE(a);
    REQUIRE(a->icon() == AlertIcon::error);
    REQUIRE(a->button_count() == 1);
    REQUIRE(a->button_label(0) == "OK");
}

TEST_CASE("AlertWindow show forces at least one button when none added",
          "[view][window-classes][alert-window][item-6-5]") {
    ScopedNoWindowFactory no_factory;
    AlertWindow a("T", "M");
    REQUIRE(a.button_count() == 0);
    // show() returns false without a factory, but should have ensured a
    // default OK button exists before returning.
    (void)a.show();
    REQUIRE(a.button_count() == 1);
    REQUIRE(a.button_label(0) == "OK");
}

// ── PR #3006 regression: native close routing ──────────────────────────────
//
// On Apple platforms `WindowHost::create` ignores the registered factory
// entirely and always returns the native NSWindow-backed host (see
// core/view/platform/mac/window_host_mac.mm:2709). The fake-factory test
// pattern below only runs on non-Apple platforms where WindowHost::create
// honors the factory. The macOS-native variant of the same regression is
// covered by the AppKit window controller's close-routing path
// (apple/AppKit*).
#if !defined(__APPLE__)

namespace {

// Minimal WindowHost that captures whatever the high-level wrapper installs
// as close_callback so the test can trigger the simulated native close.
// Implements only the pure-virtual surface so a unique_ptr to it satisfies
// WindowHost::Factory.
class FakeWindowHost final : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return false; }
    void repaint() override {}
    void set_close_callback(std::function<void()> cb) override {
        close_callback_ = std::move(cb);
    }
    void run_event_loop() override {}

    // Simulate a native title-bar / Cmd+W close arriving on this host.
    void fire_native_close() {
        if (close_callback_) close_callback_();
    }

private:
    std::function<void()> close_callback_;
};

// Install the fake factory and remember the most recently created host so
// the test can poke its native-close path. Scoped via RAII to keep the
// global factory state local to the test case.
struct ScopedFakeWindowFactory {
    ScopedFakeWindowFactory() {
        WindowHost::set_factory(
            [this](View&, const WindowOptions&)
                -> std::unique_ptr<WindowHost> {
                auto host = std::make_unique<FakeWindowHost>();
                last_host = host.get();
                return host;
            });
    }
    ~ScopedFakeWindowFactory() {
        last_host = nullptr;
        WindowHost::clear_factory();
    }
    FakeWindowHost* last_host = nullptr;
};

} // namespace

TEST_CASE("DocumentWindow::show() routes native closes through the "
          "confirmation handler (regression: PR #3006 review)",
          "[view][window-classes][document-window][issue-3006]") {
    ScopedFakeWindowFactory factory;
    View root;
    DocumentWindow doc("Doc", root);

    int hook_calls = 0;
    bool allow_close = false;
    doc.set_close_confirmation_handler([&] {
        ++hook_calls;
        return allow_close;
    });

    REQUIRE(doc.show());
    REQUIRE(factory.last_host != nullptr);

    // Simulate a native title-bar close. The hook MUST run (otherwise the
    // bug Codex flagged stands — close affordances bypass the hook).
    factory.last_host->fire_native_close();
    REQUIRE(hook_calls == 1);

    // Toggle the hook decision and fire again to prove subsequent closes
    // re-enter the hook (not a one-shot signal).
    allow_close = true;
    factory.last_host->fire_native_close();
    REQUIRE(hook_calls == 2);
}

TEST_CASE("DialogWindow::show() emits closed via the completion handler "
          "when the native window closes (regression: PR #3006 review)",
          "[view][window-classes][dialog-window][issue-3006]") {
    ScopedFakeWindowFactory factory;
    View content;
    DialogWindow dlg("Dlg", content);

    std::optional<DialogResult> received;
    dlg.set_completion_handler([&](DialogResult r) { received = r; });

    REQUIRE(dlg.show());
    REQUIRE(factory.last_host != nullptr);

    // Simulate the user closing the dialog via native chrome.
    factory.last_host->fire_native_close();
    REQUIRE(dlg.is_dismissed());
    REQUIRE(dlg.last_result() == DialogResult::closed);
    REQUIRE(received == DialogResult::closed);
}

#endif // !defined(__APPLE__)
