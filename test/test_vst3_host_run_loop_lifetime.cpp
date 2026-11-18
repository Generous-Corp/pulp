// The VST3 host run loop's handlers when a host callback destroys the loop
// from inside itself (a tick that closes the editor). Freed memory is filled
// with a poison pattern, so a handler that touches the loop or its own object
// after the callback returns reads garbage and fails, with or without a
// sanitizer.

#include "vst3_host_run_loop.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include <malloc.h>

void* operator new(std::size_t size) {
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    if (p != nullptr) {
        std::memset(p, 0xDD, malloc_usable_size(p));
        std::free(p);
    }
}
void operator delete(void* p, std::size_t) noexcept {
    ::operator delete(p);
}

using namespace Steinberg;
using pulp::format::vst3::HostRunLoop;

namespace {

class FakeRunLoop final : public Linux::IRunLoop {
  public:
    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* h,
                                            Linux::FileDescriptor fd) override {
        handlers.push_back({h, fd});
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* h) override {
        for (auto it = handlers.begin(); it != handlers.end(); ++it)
            if (it->first == h) {
                handlers.erase(it);
                return kResultTrue;
            }
        return kResultFalse;
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* h, Linux::TimerInterval) override {
        timers.push_back(h);
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* h) override {
        for (auto it = timers.begin(); it != timers.end(); ++it)
            if (*it == h) {
                timers.erase(it);
                return kResultTrue;
            }
        return kResultFalse;
    }
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override {
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override {
        return 1;
    }
    uint32 PLUGIN_API release() override {
        return 1;
    }
    std::vector<std::pair<Linux::IEventHandler*, int>> handlers;
    std::vector<Linux::ITimerHandler*> timers;
};

} // namespace

TEST_CASE("a timer tick that destroys the loop leaves nothing touched afterwards",
          "[vst3][host-run-loop][lifetime]") {
    FakeRunLoop host;
    auto loop = std::make_unique<HostRunLoop>(&host);
    int ticks = 0;
    REQUIRE(loop->add_timer(std::chrono::milliseconds(10), [&] {
        ++ticks;
        loop.reset(); // the editor closed from its own tick
    }) != 0);
    REQUIRE(host.timers.size() == 1);
    host.timers.front()->onTimer();
    REQUIRE(ticks == 1);
    REQUIRE(loop == nullptr);
    REQUIRE(host.timers.empty()); // withdrawn from the host
}

TEST_CASE("an fd callback that destroys the loop leaves nothing touched afterwards",
          "[vst3][host-run-loop][lifetime]") {
    FakeRunLoop host;
    auto loop = std::make_unique<HostRunLoop>(&host);
    int calls = 0;
    REQUIRE(loop->add_fd(7, [&] {
        ++calls;
        loop.reset();
    }) != 0);
    REQUIRE(host.handlers.size() == 1);
    host.handlers.front().first->onFDIsSet(7);
    REQUIRE(calls == 1);
    REQUIRE(loop == nullptr);
    REQUIRE(host.handlers.empty());
}

TEST_CASE("a tick that removes its own timer is not freed under itself",
          "[vst3][host-run-loop][lifetime]") {
    FakeRunLoop host;
    HostRunLoop loop(&host);
    std::uint64_t id = 0;
    int ticks = 0;
    id = loop.add_timer(std::chrono::milliseconds(10), [&] {
        ++ticks;
        loop.remove_timer(id);
    });
    host.timers.front()->onTimer();
    REQUIRE(ticks == 1);
    REQUIRE(host.timers.empty());
}
