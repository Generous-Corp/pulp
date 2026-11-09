// Asynchronous alerts, file panels and open_url: one completion, delivered on
// the main thread after the call returns, never after the owning scope dies;
// and real NSAlert / NSOpenPanel sheets on an offscreen window.

#include <pulp/platform/alert.hpp>
#include <pulp/platform/file_dialog.hpp>
#include <pulp/platform/main_thread.hpp>
#include <pulp/platform/open_url.hpp>

#include <catch2/catch_test_macros.hpp>

#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::platform;
using namespace std::chrono_literals;

namespace {

template <typename Pred> bool pump_until(Pred done, std::chrono::milliseconds limit = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    }
    return true;
}

void pump_for(std::chrono::milliseconds span) {
    const auto deadline = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
}

// An offscreen, never-activated window with a content view to parent to.
struct OffscreenWindow {
    NSWindow* window = nil;
    NSView* view = nil;
    OffscreenWindow() {
        [NSApplication sharedApplication];
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(-4000, -4000, 480, 320)
                                             styleMask:NSWindowStyleMaskTitled
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        window.releasedWhenClosed = NO;
        view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 480, 320)];
        window.contentView = view;
        [window orderFrontRegardless];
    }
    ~OffscreenWindow() {
        if (window.attachedSheet != nil)
            [window endSheet:window.attachedSheet];
        [window orderOut:nil];
        [view release];
        [window release];
    }
    void* parent() const {
        return (__bridge void*)view;
    }
};

struct RecordingPresenter {
    int presented = 0;
    int cancelled = 0;
    std::function<void(long)> respond;
    AlertButtonPlan plan;
    void install() {
        set_alert_presenter({
            [this](const AlertRequest&, const AlertButtonPlan& p, std::function<void(long)> r) {
                ++presented;
                plan = p;
                respond = std::move(r);
            },
            [this] { ++cancelled; },
        });
    }
    ~RecordingPresenter() {
        set_alert_presenter({});
    }
};

} // namespace

TEST_CASE("alert button plans map buttons to results and Escape", "[platform][alert]") {
    AlertRequest request;
    request.buttons = AlertButtons::ok;
    auto ok = alert_button_plan(request);
    REQUIRE(ok.titles == std::vector<std::string>{"OK"});
    REQUIRE(alert_result_for_button(ok, 0) == AlertResult::ok);

    request.buttons = AlertButtons::ok_cancel;
    auto oc = alert_button_plan(request);
    REQUIRE(oc.titles == std::vector<std::string>{"OK", "Cancel"});
    REQUIRE(oc.escape_index == 1);
    REQUIRE(alert_result_for_button(oc, 0) == AlertResult::ok);
    REQUIRE(alert_result_for_button(oc, 1) == AlertResult::cancel);
    REQUIRE(alert_result_for_button(oc, -1000) == AlertResult::cancel); // abort → Escape

    request.buttons = AlertButtons::yes_no;
    request.button_labels = {"Replace", "Keep"};
    auto yn = alert_button_plan(request);
    REQUIRE(yn.titles == std::vector<std::string>{"Replace", "Keep"});
    REQUIRE(alert_result_for_button(yn, 0) == AlertResult::yes);
    REQUIRE(alert_result_for_button(yn, 1) == AlertResult::no);
    REQUIRE(alert_result_for_button(yn, 7) == AlertResult::no);

    request.button_labels = {"only one"}; // wrong count: defaults stay
    REQUIRE(alert_button_plan(request).titles == std::vector<std::string>{"Yes", "No"});
}

TEST_CASE("an alert's result arrives once, after the call returns, on the main thread",
          "[platform][alert][presenter]") {
    RecordingPresenter presenter;
    presenter.install();
    MainThreadScope scope;
    int calls = 0;
    AlertResult got = AlertResult::unavailable;
    bool on_main = false;
    AlertRequest request;
    request.buttons = AlertButtons::ok_cancel;
    auto session = show_alert_async(scope, nullptr, request, [&](AlertResult r) {
        ++calls;
        got = r;
        on_main = is_main_thread();
    });
    REQUIRE(presenter.presented == 1);
    REQUIRE(session.open());
    presenter.respond(1); // synchronous response from the presenter
    REQUIRE(calls == 0);  // never inside the responding call
    presenter.respond(0); // a second response is ignored
    REQUIRE(pump_until([&] { return calls > 0; }));
    pump_for(30ms);
    REQUIRE(calls == 1);
    REQUIRE(got == AlertResult::cancel);
    REQUIRE(on_main);
    REQUIRE_FALSE(session.open());
}

TEST_CASE("destroying the scope closes an open alert and drops its completion",
          "[platform][alert][presenter]") {
    RecordingPresenter presenter;
    presenter.install();
    int calls = 0;
    {
        MainThreadScope scope;
        AlertRequest request;
        request.buttons = AlertButtons::yes_no;
        show_alert_async(scope, nullptr, request, [&](AlertResult) { ++calls; });
        REQUIRE(presenter.presented == 1);
    }
    REQUIRE(presenter.cancelled == 1);
    pump_for(50ms);
    REQUIRE(calls == 0);
}

TEST_CASE("an alert with no window to attach to reports unavailable asynchronously",
          "[platform][alert]") {
    [NSApplication sharedApplication];
    MainThreadScope scope;
    NSView* orphan = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
    std::optional<AlertResult> got;
    show_alert_async(scope, (__bridge void*)orphan, AlertRequest{},
                     [&](AlertResult r) { got = r; });
    REQUIRE_FALSE(got.has_value());
    REQUIRE(pump_until([&] { return got.has_value(); }));
    REQUIRE(*got == AlertResult::unavailable);
    [orphan release];
}

TEST_CASE("a native alert is a sheet on the parent's window and maps its buttons",
          "[platform][alert][native]") {
    OffscreenWindow host;
    MainThreadScope scope;
    std::optional<AlertResult> got;
    AlertRequest request;
    request.title = "Replace patch?";
    request.message = "A patch with this name exists.";
    request.buttons = AlertButtons::yes_no;
    request.style = AlertStyle::warning;
    show_alert_async(scope, host.parent(), request, [&](AlertResult r) { got = r; });
    REQUIRE(pump_until([&] { return host.window.attachedSheet != nil; }));
    NSWindow* sheet = host.window.attachedSheet;
    [host.window endSheet:sheet returnCode:NSAlertSecondButtonReturn];
    REQUIRE(pump_until([&] { return got.has_value(); }));
    REQUIRE(*got == AlertResult::no);

    got.reset();
    show_alert_async(scope, host.parent(), request, [&](AlertResult r) { got = r; });
    REQUIRE(pump_until([&] { return host.window.attachedSheet != nil; }));
    [host.window endSheet:host.window.attachedSheet returnCode:NSAlertFirstButtonReturn];
    REQUIRE(pump_until([&] { return got.has_value(); }));
    REQUIRE(*got == AlertResult::yes);
}

TEST_CASE("cancelling a native alert gives the Escape result", "[platform][alert][native]") {
    OffscreenWindow host;
    MainThreadScope scope;
    std::optional<AlertResult> got;
    AlertRequest request;
    request.buttons = AlertButtons::ok_cancel;
    auto session = show_alert_async(scope, host.parent(), request, [&](AlertResult r) { got = r; });
    REQUIRE(pump_until([&] { return host.window.attachedSheet != nil; }));
    session.cancel();
    REQUIRE(pump_until([&] { return got.has_value(); }));
    REQUIRE(*got == AlertResult::cancel);
    REQUIRE(pump_until([&] { return host.window.attachedSheet == nil; }));
}

TEST_CASE("destroying the scope ends a native alert sheet without a completion",
          "[platform][alert][native]") {
    OffscreenWindow host;
    int calls = 0;
    {
        MainThreadScope scope;
        show_alert_async(scope, host.parent(), AlertRequest{}, [&](AlertResult) { ++calls; });
        REQUIRE(pump_until([&] { return host.window.attachedSheet != nil; }));
    }
    REQUIRE(pump_until([&] { return host.window.attachedSheet == nil; }));
    pump_for(50ms);
    REQUIRE(calls == 0);
}

TEST_CASE("filters map to content types, keeping extensions the system does not know",
          "[platform][file-dialog]") {
    const auto ids = FileDialog::content_type_ids_for(
        {{"Audio", "wav;flac"}, {"Patches", ".syx;pulpunknownext"}});
    REQUIRE(ids.size() == 4);
    REQUIRE(std::find(ids.begin(), ids.end(), "com.microsoft.waveform-audio") != ids.end());
    REQUIRE(std::find(ids.begin(), ids.end(), "org.xiph.flac") != ids.end());
    const auto dynamic = std::count_if(
        ids.begin(), ids.end(), [](const std::string& id) { return id.rfind("dyn.", 0) == 0; });
    REQUIRE(dynamic >= 1);
}

TEST_CASE("a native open panel is configured from the request and cancels to empty",
          "[platform][file-dialog][native]") {
    OffscreenWindow host;
    MainThreadScope scope;
    int calls = 0;
    std::vector<std::string> got{"sentinel"};
    FileDialog::OpenRequest request;
    request.choose_files = false;
    request.choose_directories = true;
    request.multiple = true;
    auto session =
        FileDialog::open_async(scope, host.parent(), request, [&](std::vector<std::string> paths) {
            ++calls;
            got = std::move(paths);
        });
    REQUIRE(pump_until([&] { return host.window.attachedSheet != nil; }));
    REQUIRE([host.window.attachedSheet isKindOfClass:[NSOpenPanel class]]);
    NSOpenPanel* panel = static_cast<NSOpenPanel*>(host.window.attachedSheet);
    REQUIRE(panel.canChooseDirectories);
    REQUIRE_FALSE(panel.canChooseFiles);
    REQUIRE(panel.allowsMultipleSelection);
    session.cancel();
    REQUIRE(pump_until([&] { return calls > 0; }));
    pump_for(30ms);
    REQUIRE(calls == 1);
    REQUIRE(got.empty());
}

TEST_CASE("an async backend serves open_async; its completion may come from any thread",
          "[platform][file-dialog][backend]") {
    std::thread worker;
    FileDialog::AsyncBackend backend;
    backend.open = [&](const FileDialog::OpenRequest&,
                       std::function<void(std::vector<std::string>)> complete) {
        worker = std::thread([complete] {
            complete({"/tmp/a.syx"});
            complete({"/tmp/b.syx"}); // ignored
        });
    };
    FileDialog::set_async_backend(backend);
    MainThreadScope scope;
    int calls = 0;
    bool on_main = false;
    std::vector<std::string> got;
    FileDialog::open_async(scope, nullptr, {}, [&](std::vector<std::string> paths) {
        ++calls;
        on_main = is_main_thread();
        got = std::move(paths);
    });
    REQUIRE(pump_until([&] { return calls > 0; }));
    worker.join();
    pump_for(30ms);
    FileDialog::clear_async_backend();
    REQUIRE(calls == 1);
    REQUIRE(on_main);
    REQUIRE(got == std::vector<std::string>{"/tmp/a.syx"});
}

TEST_CASE("destroying the scope cancels a backend dialog and drops the completion",
          "[platform][file-dialog][backend]") {
    int cancelled = 0;
    FileDialog::AsyncBackend backend;
    backend.save = [](const FileDialog::SaveRequest&,
                      std::function<void(std::optional<std::string>)>) {};
    backend.cancel = [&] { ++cancelled; };
    FileDialog::set_async_backend(backend);
    int calls = 0;
    {
        MainThreadScope scope;
        FileDialog::save_async(scope, nullptr, {}, [&](std::optional<std::string>) { ++calls; });
    }
    pump_for(50ms);
    FileDialog::clear_async_backend();
    REQUIRE(cancelled == 1);
    REQUIRE(calls == 0);
}

TEST_CASE("save_copy_async copies to the chosen destination and reports failures",
          "[platform][file-dialog][backend]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "pulp-save-copy-async-test";
    fs::create_directories(dir);
    const fs::path source = dir / "source.syx";
    const fs::path destination = dir / "destination.syx";
    fs::remove(destination);
    {
        std::ofstream out(source, std::ios::binary);
        out << "F0 7E F7";
    }
    FileDialog::AsyncBackend backend;
    backend.save = [&](const FileDialog::SaveRequest&,
                       std::function<void(std::optional<std::string>)> complete) {
        complete(destination.string());
    };
    FileDialog::set_async_backend(backend);
    MainThreadScope scope;

    std::optional<std::optional<std::string>> got;
    std::string error;
    FileDialog::save_copy_async(scope, nullptr, source.string(), {},
                                [&](std::optional<std::string> path, std::string e) {
                                    got = std::move(path);
                                    error = std::move(e);
                                });
    REQUIRE(pump_until([&] { return got.has_value(); }));
    REQUIRE(got->has_value());
    REQUIRE(**got == destination.string());
    REQUIRE(error.empty());
    REQUIRE(fs::file_size(destination) == fs::file_size(source));

    got.reset();
    FileDialog::save_copy_async(scope, nullptr, (dir / "missing.syx").string(), {},
                                [&](std::optional<std::string> path, std::string e) {
                                    got = std::move(path);
                                    error = std::move(e);
                                });
    REQUIRE(pump_until([&] { return got.has_value(); }));
    FileDialog::clear_async_backend();
    REQUIRE_FALSE(got->has_value());
    REQUIRE_FALSE(error.empty());
    fs::remove_all(dir);
}

TEST_CASE("open_url accepts web and mail links only", "[platform][open-url]") {
    REQUIRE(open_url_scheme_allowed("https://example.com/manual"));
    REQUIRE(open_url_scheme_allowed("HTTP://example.com"));
    REQUIRE(open_url_scheme_allowed("mailto:support@example.com"));
    REQUIRE_FALSE(open_url_scheme_allowed("file:///etc/passwd"));
    REQUIRE_FALSE(open_url_scheme_allowed("file://server/share/patch.syx"));
    REQUIRE_FALSE(open_url_scheme_allowed("ftp://example.com/patches"));
    REQUIRE_FALSE(open_url_scheme_allowed("javascript:alert(1)"));
    REQUIRE_FALSE(open_url_scheme_allowed("https:/missing-slash"));
    REQUIRE_FALSE(open_url_scheme_allowed("https:///no-host"));
    REQUIRE_FALSE(open_url_scheme_allowed("mailto:"));
    REQUIRE_FALSE(open_url_scheme_allowed("no-scheme"));
    REQUIRE_FALSE(open_url_scheme_allowed(""));

    std::vector<std::string> opened;
    bool succeed = true;
    set_url_opener([&](const std::string& url) {
        opened.push_back(url);
        return succeed;
    });
    REQUIRE(open_url("https://example.com") == OpenUrlResult::opened);
    REQUIRE(open_url("file:///tmp") == OpenUrlResult::refused_scheme);
    succeed = false;
    REQUIRE(open_url("https://example.org") == OpenUrlResult::failed);
    set_url_opener({});
    REQUIRE(opened == std::vector<std::string>{"https://example.com", "https://example.org"});
}
