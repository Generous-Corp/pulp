// #302: NetworkServiceDiscovery core dispatches through an installed
// Backend. Without one, every op is an honest no-op — browse() does
// NOT fake-success (the pre-#302 stub did), register_service()
// returns false, notify_service_found doesn't arrive because no
// backend is running.

#include <catch2/catch_test_macros.hpp>
#include <pulp/events/volume_detector.hpp>

#include <memory>
#include <string>
#include <vector>

using pulp::events::NetworkServiceDiscovery;

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

    void browse(std::string_view t, NetworkServiceDiscovery& o) override {
        log->browse_types.emplace_back(t);
        owner = &o;
    }
    void stop() override { log->stopped++; }
    bool register_service(std::string_view n, std::string_view t, uint16_t p) override {
        log->registered.emplace_back(std::string(n), std::string(t), p);
        return true;
    }
    void unregister_service() override { log->unregistered++; }
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
