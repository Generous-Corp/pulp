// #302: NetworkServiceDiscovery core dispatches through an installed
// Backend. Without one, every op is an honest no-op — browse() does
// NOT fake-success (the pre-#302 stub did), register_service()
// returns false, notify_service_found doesn't arrive because no
// backend is running.

#include <catch2/catch_test_macros.hpp>
#include <pulp/events/volume_detector.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

using pulp::events::LockingAsyncUpdater;
using pulp::events::MountedVolumeListChangeDetector;
using pulp::events::NetworkServiceDiscovery;
using namespace std::chrono_literals;

TEST_CASE("NSD without backend is honest no-op, not fake-success",
          "[events][service-discovery][issue-302]") {
    NetworkServiceDiscovery nsd;
    REQUIRE_FALSE(nsd.has_backend());

    // browse() with no backend must NOT set running_ — pre-#302 bug.
    nsd.browse("_http._tcp");

    // register_service() returns false explicitly.
    REQUIRE_FALSE(nsd.register_service("my-service", "_http._tcp", 8080));

    // unregister_service() is safe to call even without a backend.
    nsd.unregister_service();

    // No services discovered.
    REQUIRE(nsd.discovered().empty());
}

namespace {

// Fake backend that records which core ops were invoked and can
// push fake discoveries back through the dispatcher.
class FakeBackend : public NetworkServiceDiscovery::Backend {
public:
    struct Log {
        std::vector<std::string> browse_types;
        std::vector<std::tuple<std::string, std::string, uint16_t>> registered;
        int unregistered = 0;
        int stopped = 0;
    };
    std::shared_ptr<Log> log = std::make_shared<Log>();

    NetworkServiceDiscovery* owner = nullptr;
    bool register_result = true;

    void browse(std::string_view t, NetworkServiceDiscovery& o) override {
        log->browse_types.emplace_back(t);
        owner = &o;
    }
    void stop() override { log->stopped++; }
    bool register_service(std::string_view n, std::string_view t, uint16_t p) override {
        log->registered.emplace_back(std::string(n), std::string(t), p);
        return register_result;
    }
    void unregister_service() override { log->unregistered++; }
};

class RecordingLockingUpdater : public LockingAsyncUpdater {
public:
    void handle_async_update() override {
        handles.fetch_add(1);
    }

    std::atomic<int> handles{0};
};

} // namespace

TEST_CASE("NSD dispatches browse/register/unregister to installed backend",
          "[events][service-discovery][issue-302]") {
    NetworkServiceDiscovery nsd;
    auto backend = std::make_unique<FakeBackend>();
    auto log = backend->log;

    nsd.install_backend(std::move(backend));
    REQUIRE(nsd.has_backend());

    nsd.browse("_pulp._tcp");
    REQUIRE(log->browse_types.size() == 1);
    REQUIRE(log->browse_types.front() == "_pulp._tcp");

    REQUIRE(nsd.register_service("svc", "_pulp._tcp", 1234));
    REQUIRE(log->registered.size() == 1);
    REQUIRE(std::get<0>(log->registered.front()) == "svc");
    REQUIRE(std::get<2>(log->registered.front()) == 1234);

    nsd.unregister_service();
    REQUIRE(log->unregistered == 1);

    nsd.stop();
    REQUIRE(log->stopped == 1);
}

TEST_CASE("NSD can browse again after stop",
          "[events][service-discovery][codecov]") {
    NetworkServiceDiscovery nsd;
    auto backend = std::make_unique<FakeBackend>();
    auto log = backend->log;
    nsd.install_backend(std::move(backend));

    nsd.browse("_pulp._tcp");
    nsd.stop();
    nsd.browse("_http._tcp");
    nsd.stop();

    REQUIRE(log->browse_types == std::vector<std::string>{"_pulp._tcp", "_http._tcp"});
    REQUIRE(log->stopped == 2);
}

TEST_CASE("NSD unregister after backend removal is a no-op",
          "[events][service-discovery][codecov]") {
    NetworkServiceDiscovery nsd;
    auto backend = std::make_unique<FakeBackend>();
    auto log = backend->log;
    nsd.install_backend(std::move(backend));

    nsd.unregister_service();
    REQUIRE(log->unregistered == 1);

    nsd.install_backend(nullptr);
    nsd.unregister_service();
    REQUIRE(log->unregistered == 1);
}

TEST_CASE("NSD browse backend can publish through the dispatcher",
          "[events][service-discovery][issue-642]") {
    NetworkServiceDiscovery nsd;
    auto backend = std::make_unique<FakeBackend>();
    auto log = backend->log;
    FakeBackend* raw_backend = backend.get();

    nsd.install_backend(std::move(backend));

    std::vector<std::string> found;
    nsd.on_service_found = [&](const NetworkServiceDiscovery::Service& s) {
        found.push_back(s.name + "@" + s.hostname);
    };

    nsd.browse("_pulp._tcp");
    REQUIRE(raw_backend->owner == &nsd);
    REQUIRE(log->browse_types == std::vector<std::string>{"_pulp._tcp"});

    NetworkServiceDiscovery::Service s;
    s.name = "pulpd";
    s.type = "_pulp._tcp";
    s.hostname = "pulpd.local";
    s.address = "127.0.0.1";
    s.port = 4321;
    raw_backend->owner->notify_service_found(s);

    REQUIRE(found == std::vector<std::string>{"pulpd@pulpd.local"});
    REQUIRE(nsd.discovered().size() == 1);
    REQUIRE(nsd.discovered().front().hostname == "pulpd.local");
}

TEST_CASE("NSD forwards discovered services via on_service_found",
          "[events][service-discovery][issue-302]") {
    NetworkServiceDiscovery nsd;
    nsd.install_backend(std::make_unique<FakeBackend>());

    std::vector<std::string> found;
    nsd.on_service_found = [&](const NetworkServiceDiscovery::Service& s) {
        found.push_back(s.name);
    };

    NetworkServiceDiscovery::Service s;
    s.name = "pulpd"; s.type = "_pulp._tcp"; s.port = 2222;
    nsd.notify_service_found(s);
    REQUIRE(found == std::vector<std::string>{"pulpd"});
    REQUIRE(nsd.discovered().size() == 1);

    // Duplicate notify: dedup, no double-callback.
    nsd.notify_service_found(s);
    REQUIRE(found.size() == 1);
    REQUIRE(nsd.discovered().size() == 1);

    // Lost: triggers callback + removes from list.
    std::vector<std::string> lost;
    nsd.on_service_lost = [&](const NetworkServiceDiscovery::Service& s) {
        lost.push_back(s.name);
    };
    nsd.notify_service_lost(s);
    REQUIRE(lost == std::vector<std::string>{"pulpd"});
    REQUIRE(nsd.discovered().empty());

    // Lost again: no-op (not in list).
    nsd.notify_service_lost(s);
    REQUIRE(lost.size() == 1);
}

TEST_CASE("NSD preserves backend failure and no-callback discovery paths",
          "[events][service-discovery][issue-642]") {
    NetworkServiceDiscovery nsd;
    auto backend = std::make_unique<FakeBackend>();
    auto log = backend->log;
    backend->register_result = false;
    nsd.install_backend(std::move(backend));

    REQUIRE_FALSE(nsd.register_service("svc", "_pulp._tcp", 5555));
    REQUIRE(log->registered.size() == 1);
    REQUIRE(std::get<0>(log->registered.front()) == "svc");
    REQUIRE(std::get<1>(log->registered.front()) == "_pulp._tcp");
    REQUIRE(std::get<2>(log->registered.front()) == 5555);

    NetworkServiceDiscovery::Service s;
    s.name = "pulpd";
    s.type = "_pulp._tcp";
    s.hostname = "old.local";
    s.address = "10.0.0.1";
    s.port = 2222;
    nsd.notify_service_found(s);
    REQUIRE(nsd.discovered().size() == 1);

    s.hostname = "new.local";
    s.address = "10.0.0.2";
    s.port = 3333;
    nsd.notify_service_found(s);
    REQUIRE(nsd.discovered().size() == 1);
    REQUIRE(nsd.discovered().front().hostname == "new.local");
    REQUIRE(nsd.discovered().front().address == "10.0.0.2");
    REQUIRE(nsd.discovered().front().port == 3333);

    nsd.notify_service_lost(s);
    REQUIRE(nsd.discovered().empty());
}

// Codex P2 on #310: re-announces with changed metadata must refresh
// the cached entry and fire on_service_found, not silently drop.
TEST_CASE("NSD refreshes cached entries when metadata changes on re-announce",
          "[events][service-discovery][issue-310]") {
    NetworkServiceDiscovery nsd;
    nsd.install_backend(std::make_unique<FakeBackend>());

    std::vector<uint16_t> found_ports;
    nsd.on_service_found = [&](const NetworkServiceDiscovery::Service& s) {
        found_ports.push_back(s.port);
    };

    NetworkServiceDiscovery::Service s;
    s.name = "pulpd"; s.type = "_pulp._tcp";
    s.address = "10.0.0.1"; s.port = 2222;
    nsd.notify_service_found(s);

    // Identical re-announce: dedup, no callback.
    nsd.notify_service_found(s);
    REQUIRE(found_ports == std::vector<uint16_t>{2222});

    // Port change: refresh + callback.
    s.port = 3333;
    nsd.notify_service_found(s);
    REQUIRE(found_ports == std::vector<uint16_t>{2222, 3333});
    REQUIRE(nsd.discovered().size() == 1);
    REQUIRE(nsd.discovered().front().port == 3333);

    // Address change: refresh + callback.
    s.address = "10.0.0.2";
    nsd.notify_service_found(s);
    REQUIRE(found_ports.size() == 3);
    REQUIRE(nsd.discovered().front().address == "10.0.0.2");
}

// Codex P2 on #310: swapping backends must clear the cache so a
// stale discovery from the previous backend doesn't leak into
// queries against the new one. on_service_lost should fire for
// each evicted entry so subscribers can react.
TEST_CASE("NSD clears discoveries and fires on_service_lost when swapping backends",
          "[events][service-discovery][issue-310]") {
    NetworkServiceDiscovery nsd;
    nsd.install_backend(std::make_unique<FakeBackend>());

    std::vector<std::string> lost;
    nsd.on_service_lost = [&](const NetworkServiceDiscovery::Service& s) {
        lost.push_back(s.name);
    };

    NetworkServiceDiscovery::Service a, b;
    a.name = "alpha"; a.type = "_pulp._tcp"; a.port = 1;
    b.name = "beta";  b.type = "_pulp._tcp"; b.port = 2;
    nsd.notify_service_found(a);
    nsd.notify_service_found(b);
    REQUIRE(nsd.discovered().size() == 2);

    // Swap backends: both cached entries evicted, each fires lost.
    nsd.install_backend(std::make_unique<FakeBackend>());
    REQUIRE(nsd.discovered().empty());
    REQUIRE(lost.size() == 2);
    REQUIRE((lost[0] == "alpha" || lost[0] == "beta"));
}

// Codex P2 follow-up on #314: if a subscriber's on_service_lost
// handler re-enters the NSD API during a backend swap, it must see
// the NEW backend state — not the torn-down old one.
TEST_CASE("NSD install_backend: on_service_lost re-entry sees the new backend",
          "[events][service-discovery][issue-314]") {
    NetworkServiceDiscovery nsd;
    nsd.install_backend(std::make_unique<FakeBackend>());

    NetworkServiceDiscovery::Service s;
    s.name = "alpha"; s.type = "_pulp._tcp"; s.port = 1;
    nsd.notify_service_found(s);

    bool had_backend_during_lost = false;
    nsd.on_service_lost = [&](const NetworkServiceDiscovery::Service&) {
        // At this point the swap is supposed to be complete. Probe.
        had_backend_during_lost = nsd.has_backend();
    };

    // Swap. During the eviction drain, on_service_lost runs — and
    // should observe the new backend already installed.
    nsd.install_backend(std::make_unique<FakeBackend>());
    REQUIRE(had_backend_during_lost);
}

TEST_CASE("NSD removing backend stops old backend and evicts discoveries",
          "[events][service-discovery][lifecycle]") {
    NetworkServiceDiscovery nsd;
    auto backend = std::make_unique<FakeBackend>();
    auto log = backend->log;

    nsd.install_backend(std::move(backend));
    NetworkServiceDiscovery::Service s;
    s.name = "alpha";
    s.type = "_pulp._tcp";
    s.port = 1;
    nsd.notify_service_found(s);
    REQUIRE(nsd.discovered().size() == 1);

    std::vector<std::string> lost;
    nsd.on_service_lost = [&](const NetworkServiceDiscovery::Service& svc) {
        lost.push_back(svc.name);
    };

    nsd.install_backend(nullptr);
    REQUIRE_FALSE(nsd.has_backend());
    REQUIRE(nsd.discovered().empty());
    REQUIRE(lost == std::vector<std::string>{"alpha"});
    REQUIRE(log->stopped == 1);
    REQUIRE_FALSE(nsd.register_service("svc", "_pulp._tcp", 1234));
}

TEST_CASE("NSD caches same-name services separately by type",
          "[events][service-discovery][issue-642]") {
    NetworkServiceDiscovery nsd;
    nsd.install_backend(std::make_unique<FakeBackend>());

    NetworkServiceDiscovery::Service http;
    http.name = "pulpd";
    http.type = "_http._tcp";
    http.hostname = "http.local";
    http.port = 80;

    NetworkServiceDiscovery::Service pulp = http;
    pulp.type = "_pulp._tcp";
    pulp.hostname = "pulp.local";
    pulp.port = 4321;

    nsd.notify_service_found(http);
    nsd.notify_service_found(pulp);
    REQUIRE(nsd.discovered().size() == 2);

    nsd.notify_service_lost(http);
    REQUIRE(nsd.discovered().size() == 1);
    REQUIRE(nsd.discovered().front().type == "_pulp._tcp");
    REQUIRE(nsd.discovered().front().hostname == "pulp.local");
}

TEST_CASE("NSD install_backend nullptr without discoveries only stops old backend",
          "[events][service-discovery][issue-642]") {
    NetworkServiceDiscovery nsd;
    auto backend = std::make_unique<FakeBackend>();
    auto log = backend->log;
    nsd.install_backend(std::move(backend));

    int lost_count = 0;
    nsd.on_service_lost = [&](const NetworkServiceDiscovery::Service&) {
        ++lost_count;
    };

    nsd.install_backend(nullptr);
    REQUIRE_FALSE(nsd.has_backend());
    REQUIRE(nsd.discovered().empty());
    REQUIRE(lost_count == 0);
    REQUIRE(log->stopped == 1);
    REQUIRE_FALSE(nsd.register_service("svc", "_pulp._tcp", 1234));
}

TEST_CASE("NSD stop forwards to backend even before browsing",
          "[events][service-discovery][issue-642]") {
    NetworkServiceDiscovery nsd;
    auto backend = std::make_unique<FakeBackend>();
    auto log = backend->log;
    nsd.install_backend(std::move(backend));

    nsd.stop();
    nsd.stop();

    REQUIRE(log->stopped == 2);
    REQUIRE(nsd.has_backend());
}

TEST_CASE("NSD discovery stores entries when callbacks are absent",
          "[events][service-discovery][issue-642]") {
    NetworkServiceDiscovery nsd;
    nsd.install_backend(std::make_unique<FakeBackend>());

    NetworkServiceDiscovery::Service svc;
    svc.name = "pulpd";
    svc.type = "_pulp._tcp";
    svc.hostname = "pulpd.local";
    svc.address = "127.0.0.1";
    svc.port = 4321;

    nsd.notify_service_found(svc);
    REQUIRE(nsd.discovered().size() == 1);

    nsd.notify_service_lost(svc);
    REQUIRE(nsd.discovered().empty());
}

TEST_CASE("LockingAsyncUpdater trigger_and_wait handles every call synchronously",
          "[events][service-discovery][locking-updater][issue-642]") {
    RecordingLockingUpdater updater;

    updater.trigger_and_wait();
    updater.trigger_and_wait();
    updater.trigger_and_wait();

    REQUIRE(updater.handles.load() == 3);
}

TEST_CASE("NSD backend swap clears discoveries without lost callback",
          "[events][service-discovery][issue-642]") {
    NetworkServiceDiscovery nsd;
    auto backend = std::make_unique<FakeBackend>();
    auto log = backend->log;

    nsd.install_backend(std::move(backend));

    NetworkServiceDiscovery::Service s;
    s.name = "alpha";
    s.type = "_pulp._tcp";
    s.hostname = "alpha.local";
    s.port = 1;
    nsd.notify_service_found(s);
    REQUIRE(nsd.discovered().size() == 1);

    nsd.install_backend(std::make_unique<FakeBackend>());
    REQUIRE(log->stopped == 1);
    REQUIRE(nsd.has_backend());
    REQUIRE(nsd.discovered().empty());
}

TEST_CASE("NSD removing backend with no lost handler still clears cache",
          "[events][service-discovery][lifecycle][issue-642]") {
    NetworkServiceDiscovery nsd;
    auto backend = std::make_unique<FakeBackend>();
    auto log = backend->log;

    nsd.install_backend(std::move(backend));

    NetworkServiceDiscovery::Service s;
    s.name = "alpha";
    s.type = "_pulp._tcp";
    s.port = 1;
    nsd.notify_service_found(s);
    REQUIRE(nsd.discovered().size() == 1);

    nsd.install_backend(nullptr);
    REQUIRE_FALSE(nsd.has_backend());
    REQUIRE(nsd.discovered().empty());
    REQUIRE(log->stopped == 1);
}

TEST_CASE("NSD keys discoveries and loss by service name plus type",
          "[events][service-discovery][issue-642]") {
    NetworkServiceDiscovery nsd;
    nsd.install_backend(std::make_unique<FakeBackend>());

    NetworkServiceDiscovery::Service http;
    http.name = "shared";
    http.type = "_http._tcp";
    http.hostname = "http.local";
    http.port = 80;

    NetworkServiceDiscovery::Service pulp = http;
    pulp.type = "_pulp._tcp";
    pulp.hostname = "pulp.local";
    pulp.port = 4321;

    nsd.notify_service_found(http);
    nsd.notify_service_found(pulp);
    REQUIRE(nsd.discovered().size() == 2);

    std::vector<std::string> lost;
    nsd.on_service_lost = [&](const NetworkServiceDiscovery::Service& svc) {
        lost.push_back(svc.name + "/" + svc.type);
    };

    NetworkServiceDiscovery::Service wrong_type = http;
    wrong_type.type = "_ssh._tcp";
    nsd.notify_service_lost(wrong_type);
    REQUIRE(lost.empty());
    REQUIRE(nsd.discovered().size() == 2);

    nsd.notify_service_lost(http);
    REQUIRE(lost == std::vector<std::string>{"shared/_http._tcp"});
    REQUIRE(nsd.discovered().size() == 1);
    REQUIRE(nsd.discovered().front().type == "_pulp._tcp");
}

TEST_CASE("NSD routes register and unregister to the current backend after swaps",
          "[events][service-discovery][codecov]") {
    NetworkServiceDiscovery nsd;
    auto first = std::make_unique<FakeBackend>();
    auto first_log = first->log;
    auto second = std::make_unique<FakeBackend>();
    auto second_log = second->log;

    nsd.install_backend(std::move(first));
    REQUIRE(nsd.has_backend());
    REQUIRE(nsd.register_service("first", "_pulp._tcp", 1111));
    REQUIRE(first_log->registered.size() == 1);
    REQUIRE(std::get<0>(first_log->registered.front()) == "first");

    nsd.install_backend(std::move(second));
    REQUIRE(first_log->stopped == 1);
    REQUIRE(nsd.has_backend());
    REQUIRE(nsd.register_service("second", "_pulp._tcp", 2222));
    REQUIRE(first_log->registered.size() == 1);
    REQUIRE(second_log->registered.size() == 1);
    REQUIRE(std::get<0>(second_log->registered.front()) == "second");

    nsd.unregister_service();
    REQUIRE(first_log->unregistered == 0);
    REQUIRE(second_log->unregistered == 1);
}

TEST_CASE("NSD loss matching ignores services with the same type but different name",
          "[events][service-discovery][codecov]") {
    NetworkServiceDiscovery nsd;
    nsd.install_backend(std::make_unique<FakeBackend>());

    NetworkServiceDiscovery::Service alpha;
    alpha.name = "alpha";
    alpha.type = "_pulp._tcp";
    alpha.hostname = "alpha.local";
    alpha.port = 1;

    NetworkServiceDiscovery::Service beta = alpha;
    beta.name = "beta";
    beta.hostname = "beta.local";
    beta.port = 2;

    nsd.notify_service_found(alpha);
    nsd.notify_service_found(beta);
    REQUIRE(nsd.discovered().size() == 2);

    std::vector<std::string> lost;
    nsd.on_service_lost = [&](const NetworkServiceDiscovery::Service& svc) {
        lost.push_back(svc.name);
    };

    NetworkServiceDiscovery::Service missing = alpha;
    missing.name = "missing";
    nsd.notify_service_lost(missing);
    REQUIRE(lost.empty());
    REQUIRE(nsd.discovered().size() == 2);

    nsd.notify_service_lost(alpha);
    REQUIRE(lost == std::vector<std::string>{"alpha"});
    REQUIRE(nsd.discovered().size() == 1);
    REQUIRE(nsd.discovered().front().name == "beta");
    REQUIRE(nsd.discovered().front().hostname == "beta.local");
}

TEST_CASE("MountedVolumeListChangeDetector returns a sorted platform snapshot",
          "[events][volume][lifecycle]") {
#ifdef _WIN32
    SUCCEED("Windows drive probing can throw on unavailable runner drives; covered on POSIX.");
    return;
#endif

    auto volumes = MountedVolumeListChangeDetector::get_mounted_volumes();
    REQUIRE(std::is_sorted(volumes.begin(), volumes.end()));
}

TEST_CASE("MountedVolumeListChangeDetector stop before start is idempotent",
          "[events][volume][lifecycle][issue-642]") {
    MountedVolumeListChangeDetector detector;

    REQUIRE_FALSE(detector.is_running());
    detector.stop();
    REQUIRE_FALSE(detector.is_running());
    detector.stop();
    REQUIRE_FALSE(detector.is_running());
}

TEST_CASE("MountedVolumeListChangeDetector start and stop are idempotent",
          "[events][volume][lifecycle]") {
#ifdef _WIN32
    SUCCEED("Windows drive probing can throw on unavailable runner drives; covered on POSIX.");
    return;
#endif

    MountedVolumeListChangeDetector detector;
    std::atomic<int> callbacks{0};
    detector.on_change = [&](const std::vector<std::string>& volumes) {
        REQUIRE(std::is_sorted(volumes.begin(), volumes.end()));
        callbacks.fetch_add(1);
    };

    detector.start(1ms);
    REQUIRE(detector.is_running());

    detector.start(1ms);
    REQUIRE(detector.is_running());

    detector.stop();
    REQUIRE_FALSE(detector.is_running());

    detector.stop();
    REQUIRE_FALSE(detector.is_running());
}

TEST_CASE("MountedVolumeListChangeDetector stop wakes a long poll promptly",
          "[events][volume][lifecycle][codecov]") {
#ifdef _WIN32
    SUCCEED("Windows drive probing can throw on unavailable runner drives; covered on POSIX.");
    return;
#endif

    MountedVolumeListChangeDetector detector;
    detector.start(std::chrono::hours(1));
    REQUIRE(detector.is_running());

    const auto start = std::chrono::steady_clock::now();
    detector.stop();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE_FALSE(detector.is_running());
    REQUIRE(elapsed < 500ms);
}

TEST_CASE("LockingAsyncUpdater trigger_and_wait handles synchronously",
          "[events][async_updater][locking]") {
    RecordingLockingUpdater updater;

    updater.trigger_and_wait();
    REQUIRE(updater.handles.load() == 1);

    updater.trigger_and_wait();
    REQUIRE(updater.handles.load() == 2);
}
