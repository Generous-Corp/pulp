// MainThreadScope on Apple shares one run-loop source: thousands of idle scopes
// add nothing to the main run loop, posts keep their order, a destroyed scope's
// pending posts never run, and the source goes with the last scope.

#include <pulp/platform/main_thread.hpp>

#include <catch2/catch_test_macros.hpp>

#import <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::platform;

namespace {

void spin(double seconds = 0.05) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
}

// The size of the main run loop's common-mode item set, read from its own
// description: an independent reading of the real run loop, not of this layer's
// counter. Sources added to the common modes are held there.
long main_loop_common_items() {
    CFStringRef text = CFCopyDescription(CFRunLoopGetMain());
    const CFIndex capacity =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer(static_cast<std::size_t>(capacity));
    std::string description;
    if (CFStringGetCString(text, buffer.data(), capacity, kCFStringEncodingUTF8))
        description = buffer.data();
    CFRelease(text);
    const std::size_t key = description.find("common mode items = <CFBasicHash");
    if (key == std::string::npos)
        return 0; // no common-mode items at all
    const std::size_t count = description.find("count = ", key);
    REQUIRE(count != std::string::npos);
    return std::stol(description.substr(count + 8, 12));
}

} // namespace

TEST_CASE("thousands of scopes install one run-loop source",
          "[platform][main-thread][shared-source]") {
    REQUIRE(main_thread_supported());
    REQUIRE(detail::main_thread_source_count() == 0);
    const long baseline = main_loop_common_items();

    std::vector<std::unique_ptr<MainThreadScope>> scopes;
    for (int i = 0; i < 5000; ++i)
        scopes.push_back(std::make_unique<MainThreadScope>());
    REQUIRE(detail::main_thread_source_count() == 1);
    // The run loop itself sees at most one more source, not 5000.
    const long with_scopes = main_loop_common_items();
    INFO("common items before " << baseline << ", with 5000 scopes " << with_scopes);
    REQUIRE(with_scopes - baseline <= 1);

    // One scope's post still runs, though every other scope is idle.
    int ran = 0;
    REQUIRE(scopes[4321]->post([&] { ++ran; }));
    spin();
    REQUIRE(ran == 1);

    // The source goes with the last scope.
    scopes.clear();
    REQUIRE(detail::main_thread_source_count() == 0);
    REQUIRE(main_loop_common_items() == baseline);
}

TEST_CASE("posts run in order, on the main thread, from any thread",
          "[platform][main-thread][shared-source]") {
    MainThreadScope scope;
    MainThreadScope other;
    std::vector<int> order;
    std::vector<int> other_order;
    for (int i = 0; i < 100; ++i) {
        scope.post([&order, i] { order.push_back(i); });
        other.post([&other_order, i] { other_order.push_back(i); });
    }
    spin();
    REQUIRE(order.size() == 100);
    REQUIRE(other_order.size() == 100);
    for (int i = 0; i < 100; ++i) {
        REQUIRE(order[i] == i);
        REQUIRE(other_order[i] == i);
    }

    std::atomic<bool> on_main{true};
    std::atomic<int> done{0};
    std::thread worker([&] {
        for (int i = 0; i < 20; ++i)
            scope.post([&] {
                if (!is_main_thread())
                    on_main = false;
                ++done;
            });
    });
    worker.join();
    spin();
    REQUIRE(done == 20);
    REQUIRE(on_main);

    // A task that posts again runs that post on a later pass, after its own.
    std::vector<int> nested;
    scope.post([&] {
        nested.push_back(1);
        scope.post([&] { nested.push_back(3); });
        nested.push_back(2);
    });
    spin();
    REQUIRE(nested == std::vector<int>{1, 2, 3});
}

TEST_CASE("a destroyed scope's pending posts never run", "[platform][main-thread][shared-source]") {
    int ran = 0;
    int survivor = 0;
    MainThreadScope keep;
    {
        MainThreadScope doomed;
        for (int i = 0; i < 10; ++i)
            doomed.post([&] { ++ran; });
        keep.post([&] { ++survivor; });
    }
    spin();
    REQUIRE(ran == 0);
    REQUIRE(survivor == 1); // a scope's destruction does not take others' work

    // A task that destroys its own scope drops the rest of that scope's queue.
    auto self = std::make_unique<MainThreadScope>();
    int first = 0;
    int second = 0;
    self->post([&] {
        ++first;
        self.reset();
    });
    // Posted through a handle before the destruction; never runs.
    // (the second post is queued on the same scope)
    auto* raw = self.get();
    raw->post([&] { ++second; });
    spin();
    REQUIRE(first == 1);
    REQUIRE(second == 0);
}

TEST_CASE("posting races with destruction safely", "[platform][main-thread][shared-source]") {
    for (int round = 0; round < 50; ++round) {
        auto scope = std::make_unique<MainThreadScope>();
        auto handle = scope->handle();
        std::atomic<bool> stop{false};
        std::atomic<int> ran{0};
        std::thread poster([&] {
            while (!stop)
                handle.post([&] { ++ran; });
        });
        spin(0.002);
        scope.reset();
        stop = true;
        poster.join();
        const int at_destroy = ran;
        spin(0.01);
        REQUIRE(ran == at_destroy); // nothing runs after the scope is gone
        REQUIRE_FALSE(handle.post([] {}));
    }
}
