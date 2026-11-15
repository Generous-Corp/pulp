// How an asynchronous dialog's completion reaches its caller on every
// platform: posted through the MainThreadScope where there is a main-thread
// backend, run inline where there is none (so it is never silently lost), and
// dropped once the scope is gone.

#include <pulp/platform/alert.hpp>
#include <pulp/platform/file_dialog.hpp>
#include <pulp/platform/main_thread.hpp>
#include <pulp/platform/popup_menu.hpp>

#include "dialog_internal.hpp"

#include <catch2/catch_test_macros.hpp>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <chrono>
#include <functional>
#include <optional>
#include <vector>

using namespace pulp::platform;
using detail::deliver_or_inline;
using detail::Delivery;

namespace {

// Runs the main run loop where there is one; elsewhere nothing is queued.
template <typename Pred> bool pump_until(Pred done) {
#if defined(__APPLE__)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    }
#endif
    return done();
}

} // namespace

TEST_CASE("an accepted post defers the completion", "[platform][dialog-delivery]") {
    int ran = 0;
    std::vector<std::function<void()>> queue;
    const auto how = deliver_or_inline([&] { ++ran; },
                                       [&](std::function<void()> t) {
                                           queue.push_back(std::move(t));
                                           return true;
                                       },
                                       [] { return true; });
    REQUIRE(how == Delivery::posted);
    REQUIRE(ran == 0);
    queue.front()();
    REQUIRE(ran == 1);
}

TEST_CASE("a live scope with no main-thread backend runs the completion inline",
          "[platform][dialog-delivery]") {
    int ran = 0;
    const auto how = deliver_or_inline([&] { ++ran; }, [](std::function<void()>) { return false; },
                                       [] { return true; });
    REQUIRE(how == Delivery::inline_no_backend);
    REQUIRE(ran == 1);
}

TEST_CASE("a completion for a scope that is gone is dropped", "[platform][dialog-delivery]") {
    int ran = 0;
    const auto how = deliver_or_inline([&] { ++ran; }, [](std::function<void()>) { return false; },
                                       [] { return false; });
    REQUIRE(how == Delivery::dropped);
    REQUIRE(ran == 0);
}

// The public contract on whichever platform this runs: an alert with nothing
// to show it on finishes exactly once with `unavailable`. With a main-thread
// backend that happens after the call returns; without one it happens inside
// the call. Either way it is never lost.
TEST_CASE("an alert that cannot be shown reports unavailable exactly once",
          "[platform][dialog-delivery]") {
    MainThreadScope scope;
    int calls = 0;
    std::optional<AlertResult> got;
    show_alert_async(scope, nullptr, AlertRequest{}, [&](AlertResult r) {
        ++calls;
        got = r;
    });
    if (main_thread_supported()) {
        REQUIRE(calls == 0);
        REQUIRE(pump_until([&] { return calls > 0; }));
    } else {
        REQUIRE(calls == 1);
    }
    REQUIRE(calls == 1);
    REQUIRE(got == AlertResult::unavailable);
}

TEST_CASE("a file panel with no backend reports an empty result exactly once",
          "[platform][dialog-delivery]") {
    FileDialog::clear_async_backend();
    MainThreadScope scope;
    int calls = 0;
    std::vector<std::string> got{"sentinel"};
    FileDialog::OpenRequest request;
    request.choose_files = false; // nothing selectable: no panel on any platform
    FileDialog::open_async(scope, nullptr, request, [&](std::vector<std::string> paths) {
        ++calls;
        got = std::move(paths);
    });
    REQUIRE(pump_until([&] { return calls > 0; }));
    REQUIRE(calls == 1);
    REQUIRE(got.empty());
}

// A backend or presenter that completes inside the call that opens the dialog
// leaves nothing showing. Tearing the scope down afterwards must not cancel it.
TEST_CASE("a file dialog completed synchronously is not cancelled by scope teardown",
          "[platform][dialog-delivery]") {
    int cancels = 0;
    FileDialog::AsyncBackend backend;
    backend.open = [](const FileDialog::OpenRequest&,
                      std::function<void(std::vector<std::string>)> done) { done({"a"}); };
    backend.save = [](const FileDialog::SaveRequest&,
                      std::function<void(std::optional<std::string>)> done) { done("a"); };
    backend.cancel = [&] { ++cancels; };
    FileDialog::set_async_backend(backend);
    {
        MainThreadScope scope;
        FileDialog::open_async(scope, nullptr, FileDialog::OpenRequest{},
                               [](std::vector<std::string>) {});
        FileDialog::save_async(scope, nullptr, FileDialog::SaveRequest{},
                               [](std::optional<std::string>) {});
    }
    FileDialog::clear_async_backend();
    REQUIRE(cancels == 0);
}

TEST_CASE("an alert completed synchronously is not cancelled by scope teardown",
          "[platform][dialog-delivery]") {
    int cancels = 0;
    AlertPresenter presenter;
    presenter.present = [](const AlertRequest&, const AlertButtonPlan&,
                           std::function<void(long)> respond) { respond(0); };
    presenter.cancel = [&] { ++cancels; };
    set_alert_presenter(presenter);
    {
        MainThreadScope scope;
        show_alert_async(scope, nullptr, AlertRequest{}, [](AlertResult) {});
    }
    set_alert_presenter({});
    REQUIRE(cancels == 0);
}

TEST_CASE("a menu completed synchronously is not cancelled by scope teardown",
          "[platform][dialog-delivery]") {
    int cancels = 0;
    MenuPresenter presenter;
    presenter.present = [](const MenuAnchor&, const MenuModel&, MenuPlacement,
                           std::function<void(std::optional<int>)> respond) { respond(1); };
    presenter.cancel = [&] { ++cancels; };
    set_menu_presenter(presenter);
    {
        MainThreadScope scope;
        show_menu_async(scope, MenuAnchor{}, MenuModel{}, MenuPlacement::below_anchor,
                        [](std::optional<int>) {});
    }
    set_menu_presenter({});
    REQUIRE(cancels == 0);
}

// Control: a dialog still showing when the scope dies is cancelled.
TEST_CASE("a menu still showing is cancelled by scope teardown", "[platform][dialog-delivery]") {
    int cancels = 0;
    MenuPresenter presenter;
    presenter.present = [](const MenuAnchor&, const MenuModel&, MenuPlacement,
                           std::function<void(std::optional<int>)>) {};
    presenter.cancel = [&] { ++cancels; };
    set_menu_presenter(presenter);
    {
        MainThreadScope scope;
        show_menu_async(scope, MenuAnchor{}, MenuModel{}, MenuPlacement::below_anchor,
                        [](std::optional<int>) {});
    }
    set_menu_presenter({});
    REQUIRE(cancels == 1);
}
