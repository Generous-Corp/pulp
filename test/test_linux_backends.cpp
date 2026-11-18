// Behavior tests of the Linux Avahi and libnotify backends against fake
// libraries. The backend sources are included so the tests reach their
// internals; the fakes stand in for the dlopen'd Avahi and libnotify symbols,
// so no daemon or session is needed. Linux only.

#include <catch2/catch_test_macros.hpp>

#if defined(__linux__) && !defined(__ANDROID__)

#include "../core/events/platform/linux/avahi_backend.cpp"
#include "../core/events/platform/linux/libnotify_backend.cpp"

#include <chrono>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>

namespace {

using namespace pulp::events;

// ── Fake Avahi ────────────────────────────────────────────────────────────

struct FakeAvahi {
    std::mutex poll_mutex;
    AvahiServiceBrowserCallback browse_cb = nullptr;
    void* browse_userdata = nullptr;
    AvahiServiceResolverCallback resolve_cb = nullptr;
    std::vector<void*> resolver_userdata;
    std::vector<AvahiServiceResolver*> resolvers;
    std::vector<AvahiServiceResolver*> freed_resolvers;
    int next_handle = 1;
    int poll_start_result = 0;
    int client_frees = 0;
    int poll_frees = 0;
};

FakeAvahi& fake() {
    static FakeAvahi f;
    return f;
}

AvahiThreadedPoll* poll_new() {
    return reinterpret_cast<AvahiThreadedPoll*>(0x10);
}
void poll_free(AvahiThreadedPoll*) {
    ++fake().poll_frees;
}
AvahiPoll* poll_get(AvahiThreadedPoll*) {
    return reinterpret_cast<AvahiPoll*>(0x20);
}
int poll_start(AvahiThreadedPoll*) {
    return fake().poll_start_result;
}
int poll_stop(AvahiThreadedPoll*) {
    return 0;
}
void poll_lock(AvahiThreadedPoll*) {
    fake().poll_mutex.lock();
}
void poll_unlock(AvahiThreadedPoll*) {
    fake().poll_mutex.unlock();
}
AvahiClient* client_new(const AvahiPoll*, int, AvahiClientCallback, void*, int*) {
    return reinterpret_cast<AvahiClient*>(0x30);
}
void client_free(AvahiClient*) {
    ++fake().client_frees;
}
AvahiServiceBrowser* browser_new(AvahiClient*, AvahiIfIndex, AvahiProtocol, const char*,
                                 const char*, int, AvahiServiceBrowserCallback cb, void* ud) {
    fake().browse_cb = cb;
    fake().browse_userdata = ud;
    return reinterpret_cast<AvahiServiceBrowser*>(0x40);
}
int browser_free(AvahiServiceBrowser*) {
    return 0;
}
AvahiServiceResolver* resolver_new(AvahiClient*, AvahiIfIndex, AvahiProtocol, const char*,
                                   const char*, const char*, AvahiProtocol, int,
                                   AvahiServiceResolverCallback cb, void* ud) {
    auto* handle = reinterpret_cast<AvahiServiceResolver*>(
        static_cast<uintptr_t>(0x100 + fake().next_handle++));
    fake().resolve_cb = cb;
    fake().resolver_userdata.push_back(ud);
    fake().resolvers.push_back(handle);
    return handle;
}
int resolver_free(AvahiServiceResolver* r) {
    fake().freed_resolvers.push_back(r);
    return 0;
}
char* address_snprint(char*, size_t, const AvahiAddress*) {
    return nullptr;
}

AvahiApi fake_api() {
    AvahiApi api;
    api.threaded_poll_new = poll_new;
    api.threaded_poll_free = poll_free;
    api.threaded_poll_get = poll_get;
    api.threaded_poll_start = poll_start;
    api.threaded_poll_stop = poll_stop;
    api.threaded_poll_lock = poll_lock;
    api.threaded_poll_unlock = poll_unlock;
    api.client_new = client_new;
    api.client_free = client_free;
    api.service_browser_new = browser_new;
    api.service_browser_free = browser_free;
    api.service_resolver_new = resolver_new;
    api.service_resolver_free = resolver_free;
    api.address_snprint = address_snprint;
    return api;
}

std::unique_ptr<AvahiBackend> make_backend(int poll_start_result = 0) {
    fake().browse_cb = nullptr;
    fake().resolve_cb = nullptr;
    fake().resolver_userdata.clear();
    fake().resolvers.clear();
    fake().freed_resolvers.clear();
    fake().poll_start_result = poll_start_result;
    fake().client_frees = 0;
    fake().poll_frees = 0;
    return std::make_unique<AvahiBackend>(std::make_unique<pulp::runtime::DynamicLibrary>(),
                                          fake_api());
}

// Deliver one "service appeared" browse event the way the Avahi poll thread
// does: with the poll lock held.
void deliver_new_service(const char* name) {
    std::lock_guard<std::mutex> poll(fake().poll_mutex);
    fake().browse_cb(reinterpret_cast<AvahiServiceBrowser*>(0x40), 1, 0, AVAHI_BROWSER_NEW, name,
                     "_http._tcp", "local", 0, fake().browse_userdata);
}

// Deliver the resolver result for the most recent resolver, poll lock held.
void deliver_resolved(size_t which, const char* host) {
    std::lock_guard<std::mutex> poll(fake().poll_mutex);
    fake().resolve_cb(fake().resolvers[which], 1, 0, AVAHI_RESOLVER_FOUND, "svc", "_http._tcp",
                      "local", host, nullptr, 8080, nullptr, 0, fake().resolver_userdata[which]);
}

TEST_CASE("avahi backend fails initialization when the poll thread does not start",
          "[platform][linux][backends]") {
    // Control: a started poll leaves a valid backend that owns its client and poll.
    {
        auto good = make_backend(0);
        CHECK(good->valid());
        CHECK(fake().client_frees == 0);
    }
    CHECK(fake().client_frees == 1);
    CHECK(fake().poll_frees == 1);

    // A failed start: no callback can ever run, so the backend is not valid and
    // the client and the poll are released at once, exactly once each.
    auto failed = make_backend(-1);
    CHECK_FALSE(failed->valid());
    CHECK(fake().client_frees == 1);
    CHECK(fake().poll_frees == 1);
    // Browsing and registering on an invalid backend do nothing.
    failed->stop();
    failed.reset();
    CHECK(fake().client_frees == 1); // the destructor frees nothing a second time
    CHECK(fake().poll_frees == 1);
}

TEST_CASE("avahi browse and resolver callbacks do not deadlock", "[platform][linux][backends]") {
    auto backend = make_backend();
    CHECK(backend->valid());
    NetworkServiceDiscovery owner;
    backend->browse("_http._tcp", owner);

    // The poll thread runs resolver callbacks with the poll lock held while
    // another thread starts a browse. One lock order keeps both moving.
    auto worker = std::async(std::launch::async, [&] {
        for (int i = 0; i < 3000; ++i) {
            fake().resolvers.clear();
            fake().resolver_userdata.clear();
            {
                std::lock_guard<std::mutex> poll(fake().poll_mutex);
                fake().browse_cb(reinterpret_cast<AvahiServiceBrowser*>(0x40), 1, 0,
                                 AVAHI_BROWSER_NEW, "svc", "_http._tcp", "local", 0,
                                 fake().browse_userdata);
                std::this_thread::sleep_for(std::chrono::microseconds(20));
                if (!fake().resolvers.empty()) {
                    fake().resolve_cb(fake().resolvers[0], 1, 0, AVAHI_RESOLVER_FOUND, "svc",
                                      "_http._tcp", "local", "h", nullptr, 1, nullptr, 0,
                                      fake().resolver_userdata[0]);
                }
            }
        }
    });
    auto browser = std::async(std::launch::async, [&] {
        for (int i = 0; i < 3000; ++i)
            backend->browse("_http._tcp", owner);
    });
    const auto limit = std::chrono::seconds(20);
    const bool worker_done = worker.wait_for(limit) == std::future_status::ready;
    const bool browser_done = browser.wait_for(limit) == std::future_status::ready;
    CHECK(worker_done);
    CHECK(browser_done);
    if (!worker_done || !browser_done)
        std::_Exit(1); // a deadlocked pair cannot be joined
}

TEST_CASE("avahi stop frees pending resolvers and drops their results",
          "[platform][linux][backends]") {
    auto backend = make_backend();
    NetworkServiceDiscovery owner_a;
    int found_a = 0;
    owner_a.on_service_found = [&](const NetworkServiceDiscovery::Service&) { ++found_a; };
    backend->browse("_http._tcp", owner_a);
    deliver_new_service("A");
    CHECK(fake().resolvers.size() == 1);

    // Stopping the browse frees the resolver the browse started.
    backend->stop();
    CHECK(fake().freed_resolvers.size() == 1);
    CHECK(fake().freed_resolvers[0] == fake().resolvers[0]);

    // A new browse reports only its own services.
    NetworkServiceDiscovery owner_b;
    int found_b = 0;
    owner_b.on_service_found = [&](const NetworkServiceDiscovery::Service&) { ++found_b; };
    backend->browse("_ipp._tcp", owner_b);
    deliver_new_service("B");
    CHECK(fake().resolvers.size() == 2);
    deliver_resolved(1, "b.local");
    CHECK(found_b == 1);
    CHECK(found_a == 0);
}

TEST_CASE("avahi destruction frees pending resolvers", "[platform][linux][backends]") {
    auto backend = make_backend();
    NetworkServiceDiscovery owner;
    backend->browse("_http._tcp", owner);
    deliver_new_service("A");
    deliver_new_service("B");
    CHECK(fake().resolvers.size() == 2);
    backend.reset();
    CHECK(fake().freed_resolvers.size() == 2);
}

// ── Fake libnotify ────────────────────────────────────────────────────────

struct FakeNotify {
    int init_result = 1;
    int shown = 0;
    std::vector<void*> closed;
    std::vector<void*> unrefed;
    uintptr_t next = 1;
};

FakeNotify& notify() {
    static FakeNotify f;
    return f;
}

int n_init(const char*) {
    return notify().init_result;
}
NotifyNotificationOpaque* n_new(const char*, const char*, const char*) {
    return reinterpret_cast<NotifyNotificationOpaque*>(notify().next++ * 16);
}
int n_show(NotifyNotificationOpaque*, void**) {
    ++notify().shown;
    return 1;
}
int n_close(NotifyNotificationOpaque* h, void**) {
    notify().closed.push_back(h);
    return 1;
}
void n_unref(void* h) {
    notify().unrefed.push_back(h);
}

std::unique_ptr<LibNotifyBackend> make_notify(int init_result) {
    notify() = FakeNotify{};
    notify().init_result = init_result;
    return std::make_unique<LibNotifyBackend>(pulp::runtime::DynamicLibrary{},
                                              pulp::runtime::DynamicLibrary{}, n_init, n_new,
                                              n_show, n_close, n_unref);
}

TEST_CASE("libnotify failed init is not available", "[platform][linux][backends]") {
    CHECK(!make_notify(0)->is_available());
    CHECK(make_notify(1)->is_available());
}

TEST_CASE("libnotify repeated id replaces and releases the earlier notification",
          "[platform][linux][backends]") {
    auto backend = make_notify(1);
    LocalNotification n;
    n.title = "t";
    n.id = "same";
    CHECK(backend->post_local_notification(n) != 0);
    CHECK(backend->post_local_notification(n) != 0);
    // The first handle is closed and released when the second replaces it.
    CHECK(notify().closed.size() == 1);
    CHECK(notify().unrefed.size() == 1);
    // Exactly one live notification remains, and cancelling it releases it.
    CHECK(backend->cancel_notification("same"));
    CHECK(!backend->cancel_notification("same"));
    CHECK(notify().closed.size() == 2);
    CHECK(notify().unrefed.size() == 2);
}

} // namespace

#endif // __linux__
