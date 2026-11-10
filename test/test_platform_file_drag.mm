// Outbound file drags: request validation, the recorder seam automation
// uses, the operation mapping, the drag image, the event that anchors a
// drag, and the drag source's mask and end report.

#include <pulp/platform/file_drag.hpp>
#include <pulp/platform/main_thread.hpp>

#include <catch2/catch_test_macros.hpp>

#import <Cocoa/Cocoa.h>
#include <objc/message.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>

using namespace pulp::platform;
using namespace std::chrono_literals;

namespace {

template <typename Pred> bool pump_until(Pred done, std::chrono::milliseconds limit = 2000ms) {
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

struct TempFiles {
    std::filesystem::path dir;
    std::filesystem::path a, b;
    TempFiles() {
        dir = std::filesystem::temp_directory_path() / "pulp-file-drag-test";
        std::filesystem::create_directories(dir);
        a = dir / "patch-a.syx";
        b = dir / "patch-b.syx";
        std::ofstream(a) << "a";
        std::ofstream(b) << "b";
    }
    ~TempFiles() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

struct Window {
    NSWindow* window = nil;
    NSView* view = nil;
    Window() {
        [NSApplication sharedApplication];
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(-4000, -4000, 300, 200)
                                             styleMask:NSWindowStyleMaskTitled
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        window.releasedWhenClosed = NO;
        view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 200)];
        window.contentView = view;
    }
    ~Window() {
        [view release];
        [window release];
    }
    NSEvent* event(NSEventType type) const {
        return [NSEvent mouseEventWithType:type
                                  location:NSMakePoint(50, 50)
                             modifierFlags:0
                                 timestamp:0
                              windowNumber:window.windowNumber
                                   context:nil
                               eventNumber:0
                                clickCount:1
                                  pressure:1];
    }
};

struct RecorderGuard {
    ~RecorderGuard() {
        set_file_drag_recorder({});
    }
};

} // namespace

TEST_CASE("file drag requests are validated before anything starts", "[platform][file-drag]") {
    MainThreadScope scope;
    TempFiles files;
    REQUIRE(begin_file_drag(scope, nullptr, {}, {}) == FileDragError::no_paths);
    FileDragRequest missing;
    missing.paths = {(files.dir / "nope.syx").string()};
    REQUIRE(begin_file_drag(scope, nullptr, missing, {}) == FileDragError::missing_file);
    FileDragRequest bad_image;
    bad_image.paths = {files.a.string()};
    bad_image.image = DragImage{4, 4, std::vector<std::uint8_t>(10), 1.0f, 0, 0};
    REQUIRE(begin_file_drag(scope, nullptr, bad_image, {}) == FileDragError::unavailable);
}

TEST_CASE("a recorder sees the drag automation starts and reports its operation",
          "[platform][file-drag][recorder]") {
    RecorderGuard guard;
    TempFiles files;
    std::optional<FileDragRequest> seen;
    set_file_drag_recorder([&](const FileDragRequest& request) {
        seen = request;
        return DragOperation::move;
    });
    MainThreadScope scope;
    FileDragRequest request;
    request.paths = {files.a.string(), (files.dir / "gone.syx").string(), files.b.string()};
    request.allowed = drag_copy | drag_move;
    request.image = DragImage{8, 4, std::vector<std::uint8_t>(8 * 4 * 4, 255), 2.0f, 2, 1};
    request.origin_x = 37;
    request.origin_y = 12;
    std::optional<FileDragEnd> ended;
    // No view, window or pointer event: the recorder needs none.
    REQUIRE(begin_file_drag(scope, nullptr, request, [&](FileDragEnd end) { ended = end; }) ==
            FileDragError::none);
    REQUIRE(seen.has_value());
    REQUIRE(seen->paths == std::vector<std::string>{files.a.string(), files.b.string()});
    REQUIRE(seen->allowed == (drag_copy | drag_move));
    REQUIRE(seen->image->width == 8);
    REQUIRE(seen->origin_x == 37);
    REQUIRE(seen->origin_y == 12);
    REQUIRE_FALSE(ended.has_value()); // reported later, not inside the call
    REQUIRE(pump_until([&] { return ended.has_value(); }));
    REQUIRE(ended->operation == DragOperation::move);

    int late = 0;
    {
        MainThreadScope doomed;
        REQUIRE(begin_file_drag(doomed, nullptr, request, [&](FileDragEnd) { ++late; }) ==
                FileDragError::none);
    }
    pump_for(30ms);
    REQUIRE(late == 0);
}

TEST_CASE("operations map to and from the system's drag bits", "[platform][file-drag]") {
    REQUIRE(native_drag_mask(drag_copy) == NSDragOperationCopy);
    REQUIRE(native_drag_mask(drag_move) == NSDragOperationMove);
    REQUIRE(native_drag_mask(drag_copy | drag_move) == (NSDragOperationCopy | NSDragOperationMove));
    REQUIRE(native_drag_mask(0) == NSDragOperationNone);
    REQUIRE(drag_operation_from_native(NSDragOperationCopy) == DragOperation::copy);
    REQUIRE(drag_operation_from_native(NSDragOperationMove) == DragOperation::move);
    REQUIRE(drag_operation_from_native(NSDragOperationDelete) == DragOperation::move);
    REQUIRE(drag_operation_from_native(NSDragOperationNone) == DragOperation::none);
}

TEST_CASE("a drag image is sized in points", "[platform][file-drag]") {
    @autoreleasepool {
        auto* image = (__bridge NSImage*)detail::make_drag_image(
            DragImage{64, 32, std::vector<std::uint8_t>(64 * 32 * 4, 128), 2.0f, 0, 0});
        REQUIRE(image != nil);
        REQUIRE(image.size.width == 32);
        REQUIRE(image.size.height == 16);
        REQUIRE(detail::make_drag_image(DragImage{2, 2, {}, 1.0f, 0, 0}) == nullptr);
    }
}

TEST_CASE("only a mouse-down or drag in the view's window anchors a drag",
          "[platform][file-drag][anchor]") {
    Window host;
    Window other;
    void* view = (__bridge void*)host.view;
    REQUIRE(
        detail::drag_event_anchors(view, (__bridge void*)host.event(NSEventTypeLeftMouseDragged)));
    REQUIRE(detail::drag_event_anchors(view, (__bridge void*)host.event(NSEventTypeLeftMouseDown)));
    REQUIRE(
        detail::drag_event_anchors(view, (__bridge void*)host.event(NSEventTypeRightMouseDragged)));
    REQUIRE_FALSE(
        detail::drag_event_anchors(view, (__bridge void*)host.event(NSEventTypeLeftMouseUp)));
    REQUIRE_FALSE(
        detail::drag_event_anchors(view, (__bridge void*)host.event(NSEventTypeMouseMoved)));
    REQUIRE_FALSE(
        detail::drag_event_anchors(view, (__bridge void*)other.event(NSEventTypeLeftMouseDragged)));
    REQUIRE_FALSE(detail::drag_event_anchors(view, nullptr));
}

TEST_CASE("starting a drag outside a drag event is refused, not a crash",
          "[platform][file-drag][native]") {
    Window host;
    TempFiles files;
    MainThreadScope scope;
    FileDragRequest request;
    request.paths = {files.a.string()};
    REQUIRE(begin_file_drag(scope, (__bridge void*)host.view, request, {},
                            (__bridge void*)host.event(NSEventTypeLeftMouseUp)) ==
            FileDragError::not_in_drag_event);
    NSView* orphan = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
    REQUIRE(begin_file_drag(scope, (__bridge void*)orphan, request, {}) ==
            FileDragError::no_window);
    [orphan release];
    REQUIRE(begin_file_drag(scope, nullptr, request, {}) == FileDragError::unavailable);
}

TEST_CASE("the drag source offers the allowed operations and reports the end once",
          "[platform][file-drag][source]") {
    [NSApplication sharedApplication];
    auto alive = std::make_shared<int>(0);
    std::weak_ptr<int> watch = alive;
    int ends = 0;
    std::optional<FileDragEnd> got;
    id source = (__bridge id)detail::make_file_drag_source(
        drag_copy | drag_move, [&, keep = std::move(alive)](FileDragEnd end) {
            ++ends;
            got = end;
        });
    REQUIRE(source != nil);
    REQUIRE([source conformsToProtocol:@protocol(NSDraggingSource)]);
    auto mask = reinterpret_cast<NSDragOperation (*)(id, SEL, id, NSDraggingContext)>(objc_msgSend);
    REQUIRE(mask(source, @selector(draggingSession:sourceOperationMaskForDraggingContext:), nil,
                 NSDraggingContextOutsideApplication) ==
            (NSDragOperationCopy | NSDragOperationMove));
    auto ended = reinterpret_cast<void (*)(id, SEL, id, NSPoint, NSDragOperation)>(objc_msgSend);
    [source retain]; // the test's own reference, to observe the source's
    REQUIRE([source retainCount] == 2);
    ended(source, @selector(draggingSession:endedAtPoint:operation:), nil, NSMakePoint(10, 20),
          NSDragOperationMove);
    REQUIRE(ends == 1);
    REQUIRE(got->operation == DragOperation::move);
    REQUIRE(watch.expired()); // the callback, and what it captured, is gone
    // The source gave up its own reference when its session ended.
    REQUIRE([source retainCount] == 1);
    ended(source, @selector(draggingSession:endedAtPoint:operation:), nil, NSMakePoint(0, 0),
          NSDragOperationCopy); // a second end is ignored and releases nothing
    REQUIRE(ends == 1);
    REQUIRE([source retainCount] == 1);
    [source release];

    id copy_only = (__bridge id)detail::make_file_drag_source(drag_copy, {});
    REQUIRE(mask(copy_only, @selector(draggingSession:sourceOperationMaskForDraggingContext:), nil,
                 NSDraggingContextWithinApplication) == NSDragOperationCopy);
    [copy_only release];
}
