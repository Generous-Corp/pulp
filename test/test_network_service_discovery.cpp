// #302: NetworkServiceDiscovery core dispatches through an installed
// Backend. Without one, every op is an honest no-op — browse() does
// NOT fake-success (the pre-#302 stub did), register_service()
// returns false, notify_service_found doesn't arrive because no
// backend is running.

#include <catch2/catch_test_macros.hpp>
#include <pulp/events/service_discovery.hpp>
#include <pulp/events/volume_detector.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using pulp::events::LockingAsyncUpdater;
using pulp::events::MountedVolumeListChangeDetector;
using pulp::events::NetworkServiceDiscovery;
using pulp::events::ServiceBrowser;
using pulp::events::ServiceDiscoveryAction;
using pulp::events::ServicePublisher;
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

// ── TXT records + RAII wrappers (gap-doc 2026-05-24) ────────────────────

namespace {

// FakeBackend that captures the TXT-aware register_service overload so
// tests can assert metadata round-tripped through the dispatcher.
class TxtAwareFakeBackend : public NetworkServiceDiscovery::Backend {
public:
    struct Capture {
        std::string name;
        std::string type;
        uint16_t port = 0;
        NetworkServiceDiscovery::TxtRecords txt;
    };
    std::shared_ptr<std::vector<Capture>> registrations =
        std::make_shared<std::vector<Capture>>();
    std::shared_ptr<int> stops = std::make_shared<int>(0);
    std::shared_ptr<int> unregisters = std::make_shared<int>(0);

    void browse(std::string_view, NetworkServiceDiscovery&) override {}
    void stop() override { ++(*stops); }
    bool register_service(std::string_view name,
                          std::string_view type,
                          uint16_t port) override {
        registrations->push_back({std::string(name), std::string(type), port, {}});
        return true;
    }
    bool register_service(std::string_view name,
                          std::string_view type,
                          uint16_t port,
                          const NetworkServiceDiscovery::TxtRecords& txt) override {
        registrations->push_back({std::string(name), std::string(type), port, txt});
        return true;
    }
    void unregister_service() override { ++(*unregisters); }
};

// Backend that does NOT override the TXT-aware overload so we can
// verify the default implementation drops records and forwards to the
// 3-arg form — the "degraded backend" contract from the header.
class LegacyOnlyFakeBackend : public NetworkServiceDiscovery::Backend {
public:
    std::shared_ptr<int> legacy_calls = std::make_shared<int>(0);

    void browse(std::string_view, NetworkServiceDiscovery&) override {}
    void stop() override {}
    bool register_service(std::string_view, std::string_view, uint16_t) override {
        ++(*legacy_calls);
        return true;
    }
    void unregister_service() override {}
};

}  // namespace

TEST_CASE("NSD forwards TXT records to TXT-aware backends",
          "[events][service-discovery][txt-records]") {
    NetworkServiceDiscovery nsd;
    auto backend = std::make_unique<TxtAwareFakeBackend>();
    auto registrations = backend->registrations;
    nsd.install_backend(std::move(backend));

    NetworkServiceDiscovery::TxtRecords txt{
        {"version", "1.4.2"},
        {"path",    "/api"},
    };
    REQUIRE(nsd.register_service("api", "_http._tcp", 8080, txt));
    REQUIRE(registrations->size() == 1);
    REQUIRE(registrations->front().port == 8080);
    REQUIRE(registrations->front().txt.size() == 2);
    REQUIRE(registrations->front().txt.at("version") == "1.4.2");
    REQUIRE(registrations->front().txt.at("path") == "/api");
}

TEST_CASE("NSD without backend rejects TXT-aware register_service",
          "[events][service-discovery][txt-records]") {
    NetworkServiceDiscovery nsd;
    NetworkServiceDiscovery::TxtRecords txt{{"foo", "bar"}};
    REQUIRE_FALSE(nsd.register_service("svc", "_http._tcp", 80, txt));
}

TEST_CASE("Legacy backend without TXT override drops records and accepts the call",
          "[events][service-discovery][txt-records]") {
    NetworkServiceDiscovery nsd;
    auto backend = std::make_unique<LegacyOnlyFakeBackend>();
    auto legacy_calls = backend->legacy_calls;
    nsd.install_backend(std::move(backend));

    NetworkServiceDiscovery::TxtRecords txt{{"k", "v"}};
    REQUIRE(nsd.register_service("svc", "_pulp._tcp", 1234, txt));
    REQUIRE(*legacy_calls == 1);
}

TEST_CASE("ServicePublisher RAII registers on construct, unregisters on destroy",
          "[events][service-discovery][raii]") {
    auto backend = std::make_unique<TxtAwareFakeBackend>();
    auto registrations = backend->registrations;
    auto unregisters = backend->unregisters;

    {
        auto publisher = ServicePublisher::with_backend(
            std::move(backend),
            "pulpd", "_pulp._tcp", 4321,
            NetworkServiceDiscovery::TxtRecords{{"role", "control"}});
        REQUIRE(publisher);
        REQUIRE(publisher->is_published());
        REQUIRE(registrations->size() == 1);
        REQUIRE(registrations->front().txt.at("role") == "control");
        REQUIRE(*unregisters == 0);
    }
    REQUIRE(*unregisters == 1);
}

TEST_CASE("ServicePublisher with null backend is_not_published and no-ops cleanly",
          "[events][service-discovery][raii]") {
    auto publisher = ServicePublisher::with_backend(
        nullptr, "noop", "_pulp._tcp", 1234);
    REQUIRE(publisher);
    REQUIRE_FALSE(publisher->is_published());
    // Destructor must not crash on the null-backend path.
}

namespace {

// Browse-aware backend that hands us a pointer back to the dispatcher
// so the test can drive notify_service_found / notify_service_lost
// through the wired-up callback.
class BrowsableFakeBackend : public NetworkServiceDiscovery::Backend {
public:
    NetworkServiceDiscovery* owner = nullptr;
    std::shared_ptr<std::vector<std::string>> browse_types =
        std::make_shared<std::vector<std::string>>();
    std::shared_ptr<int> stops = std::make_shared<int>(0);

    void browse(std::string_view t, NetworkServiceDiscovery& o) override {
        browse_types->emplace_back(t);
        owner = &o;
    }
    void stop() override { ++(*stops); }
    bool register_service(std::string_view, std::string_view, uint16_t) override {
        return true;
    }
    void unregister_service() override {}
};

}  // namespace

TEST_CASE("ServiceBrowser RAII starts browsing and dispatches found/lost",
          "[events][service-discovery][raii]") {
    auto backend = std::make_unique<BrowsableFakeBackend>();
    auto browse_types = backend->browse_types;
    auto stops = backend->stops;
    BrowsableFakeBackend* raw = backend.get();

    std::vector<std::pair<ServiceDiscoveryAction, std::string>> events;

    {
        auto browser = ServiceBrowser::with_backend(
            std::move(backend),
            "_pulp._tcp",
            [&](ServiceDiscoveryAction action,
                const NetworkServiceDiscovery::Service& svc) {
                events.emplace_back(action, svc.name);
            });
        REQUIRE(browser);
        REQUIRE(browser->is_browsing());
        REQUIRE(*browse_types == std::vector<std::string>{"_pulp._tcp"});
        REQUIRE(raw->owner != nullptr);

        NetworkServiceDiscovery::Service svc;
        svc.name = "alpha";
        svc.type = "_pulp._tcp";
        svc.port = 1;
        raw->owner->notify_service_found(svc);
        raw->owner->notify_service_lost(svc);

        REQUIRE(events.size() == 2);
        REQUIRE(events[0].first == ServiceDiscoveryAction::ServiceFound);
        REQUIRE(events[0].second == "alpha");
        REQUIRE(events[1].first == ServiceDiscoveryAction::ServiceLost);
        REQUIRE(events[1].second == "alpha");
    }
    // ServiceBrowser destructor calls nsd_.stop(); NSD destructor also
    // calls stop(). Either path forwards to backend->stop(), so any
    // count >= 1 is correct — what matters is that the backend got
    // stopped at least once before destruction.
    REQUIRE(*stops >= 1);
}

TEST_CASE("ServiceBrowser with null backend reports not_browsing",
          "[events][service-discovery][raii]") {
    int events = 0;
    auto browser = ServiceBrowser::with_backend(
        nullptr, "_pulp._tcp",
        [&](ServiceDiscoveryAction, const NetworkServiceDiscovery::Service&) {
            ++events;
        });
    REQUIRE(browser);
    REQUIRE_FALSE(browser->is_browsing());
    REQUIRE(events == 0);
}

TEST_CASE("install_default_backend reports platform support honestly",
          "[events][service-discovery][platform]") {
    NetworkServiceDiscovery nsd;
    const bool installed = pulp::events::install_default_backend(nsd);
#if defined(__APPLE__)
    // Apple platforms must always install a working Bonjour backend —
    // CoreServices DNS-SD is part of the OS, never optional.
    REQUIRE(installed);
    REQUIRE(nsd.has_backend());
#else
    // Linux (Avahi via libavahi-client.so.3) and Windows (Bonjour SDK
    // via dnssd.dll) resolve their backends at run-time. When the
    // host runtime library is present `installed` must be true; when
    // it isn't, `installed` must be false (honest no-mDNS, never a
    // silent no-op). Either way the dispatcher state must match the
    // return value — installed=true must mean has_backend()=true.
    REQUIRE(installed == nsd.has_backend());
#endif
}

// ── Linux Avahi + Windows Bonjour factory contracts ─────────────────────
// The platform-specific make_*_backend() factories are honest about
// runtime availability: if the host's mDNS library is installed they
// return a working backend; if not, they return nullptr so the
// dispatcher can degrade explicitly. These tests pin the contract on
// the host platform they target — both gated to their respective OS
// so the symbols are actually defined.

#if defined(__linux__)
namespace pulp::events {
std::unique_ptr<NetworkServiceDiscovery::Backend> make_avahi_backend();
}
TEST_CASE("Avahi backend factory returns nullptr or working backend",
          "[events][service-discovery][avahi][platform]") {
    // make_avahi_backend() runtime-loads libavahi-client.so.3. On
    // boxes without the library installed it must return nullptr —
    // never a half-initialized backend. When it does return a backend
    // the dispatcher must accept install_backend cleanly.
    auto backend = pulp::events::make_avahi_backend();
    if (!backend) {
        SUCCEED("libavahi-client.so.3 not present on this host; "
                "make_avahi_backend honestly returned nullptr.");
        return;
    }
    NetworkServiceDiscovery nsd;
    nsd.install_backend(std::move(backend));
    REQUIRE(nsd.has_backend());
}
#endif  // __linux__

#if defined(__APPLE__)
#include <unistd.h>  // ::getpid for unique smoke-test service name
// Smoke test against the real Bonjour stack. Publishes a unique
// service from one NSD and browses for it from another, expecting the
// browse-side dispatcher to discover it within a few seconds via the
// local mDNSResponder. Skipped when the runner explicitly opts out via
// PULP_SKIP_BONJOUR_SMOKE=1 (e.g., the sandboxed CI lane where
// mDNSResponder isn't reachable).
TEST_CASE("Bonjour backend round-trips publish → browse on macOS",
          "[events][service-discovery][bonjour][smoke][.][!mayfail]") {
    if (const char* skip = std::getenv("PULP_SKIP_BONJOUR_SMOKE"); skip && *skip) {
        SUCCEED("PULP_SKIP_BONJOUR_SMOKE set; skipping Bonjour smoke.");
        return;
    }

    const std::string unique_name =
        "pulp-nsd-smoke-" + std::to_string(::getpid());
    const std::string service_type = "_pulp-test._tcp";

    NetworkServiceDiscovery publisher_nsd;
    REQUIRE(pulp::events::install_default_backend(publisher_nsd));
    REQUIRE(publisher_nsd.register_service(unique_name, service_type, 54321,
                                            {{"v", "1"}}));

    std::atomic<bool> seen{false};
    NetworkServiceDiscovery browser_nsd;
    REQUIRE(pulp::events::install_default_backend(browser_nsd));
    browser_nsd.on_service_found =
        [&, unique_name](const NetworkServiceDiscovery::Service& svc) {
            if (svc.name == unique_name) seen.store(true);
        };
    browser_nsd.browse(service_type);

    // Poll up to 5s for the discovery; mDNSResponder usually answers
    // in well under a second on a quiet network.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!seen.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }

    publisher_nsd.unregister_service();
    browser_nsd.stop();

    REQUIRE(seen.load());
}
#endif
