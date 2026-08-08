#include <pulp/inspect/control_connection_admission.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {

ControlPeerExpectation peer(ControlPeerRole role = ControlPeerRole::Client) {
    return {.evidence = {
                .role = role,
                .user_id = "501",
                .process_id = 101,
                .process_start_id = "generation-1",
                .executable_identity = "signed:dev.pulp.test",
                .publisher_id = "adhoc:abcdef",
            }};
}

ControlConnectionPrincipal client(std::string id = "client-1") {
    return ControlClientConnectionPrincipal{ControlClientId{std::move(id)}};
}

ControlConnectionPrincipal host(std::string id = "registration-1") {
    return ControlHostConnectionPrincipal{ControlRegistrationId{std::move(id)}};
}

} // namespace

TEST_CASE("connection admissions are one-use and retain exact broker-owned bindings",
          "[inspect][control][carrier][admission][security]") {
    auto now = std::chrono::steady_clock::time_point{10s};
    ControlConnectionAdmissionStore store{{}, [&] { return now; }};

    const auto issued = store.issue(peer(), client());
    REQUIRE(issued.status == ControlConnectionAdmissionStatus::Issued);
    REQUIRE(issued.ticket);
    CHECK(issued.ticket->expires_at == now + 5s);
    CHECK(store.size() == 1);

    const auto consumed = store.consume(issued.ticket->admission_id);
    REQUIRE(consumed);
    CHECK(consumed->admission_id == issued.ticket->admission_id);
    CHECK(consumed->expected_peer.evidence.process_start_id == "generation-1");
    REQUIRE(std::holds_alternative<ControlClientConnectionPrincipal>(consumed->principal));
    CHECK(std::get<ControlClientConnectionPrincipal>(consumed->principal).client_id.value ==
          "client-1");
    CHECK_FALSE(store.consume(issued.ticket->admission_id));
    CHECK(store.size() == 0);
}

TEST_CASE("connection admissions reject malformed or role-confused bindings",
          "[inspect][control][carrier][admission][security]") {
    ControlConnectionAdmissionStore store;

    CHECK(store.issue(peer(ControlPeerRole::StandaloneHost), client()).status ==
          ControlConnectionAdmissionStatus::InvalidRequest);
    CHECK(store.issue(peer(), host()).status == ControlConnectionAdmissionStatus::InvalidRequest);
    auto malformed = peer();
    malformed.evidence.process_start_id.clear();
    CHECK(store.issue(std::move(malformed), client()).status ==
          ControlConnectionAdmissionStatus::InvalidRequest);
    CHECK(store.issue(peer(), client("")).status ==
          ControlConnectionAdmissionStatus::InvalidRequest);
    CHECK(store.size() == 0);

    ControlConnectionAdmissionStore oversized{
        {.maximum_admissions = kControlMaximumConnectionAdmissions + 1}};
    CHECK(oversized.issue(peer(), client()).status ==
          ControlConnectionAdmissionStatus::InvalidRequest);
    ControlConnectionAdmissionStore long_lived{
        {.admission_ttl = kControlMaximumConnectionAdmissionTtl + 1ms}};
    CHECK(long_lived.issue(peer(), client()).status ==
          ControlConnectionAdmissionStatus::InvalidRequest);
}

TEST_CASE("connection admission expiry and capacity fail closed",
          "[inspect][control][carrier][admission][security]") {
    auto now = std::chrono::steady_clock::time_point{20s};
    ControlConnectionAdmissionStore store{{.maximum_admissions = 1, .admission_ttl = 100ms},
                                          [&] { return now; }};

    const auto first = store.issue(peer(), client());
    REQUIRE(first.ticket);
    CHECK_FALSE(store.consume(std::string(4096, 'a')));
    CHECK_FALSE(store.consume("admission-00000000000000000000000000000000"));
    CHECK(store.size() == 1);
    CHECK(store.issue(peer(), client("client-2")).status ==
          ControlConnectionAdmissionStatus::ResourceExhausted);
    now += 100ms;
    CHECK_FALSE(store.consume(first.ticket->admission_id));
    CHECK(store.size() == 0);
    CHECK(store.issue(peer(), client("client-2")).status ==
          ControlConnectionAdmissionStatus::Issued);
}

TEST_CASE("connection admission consumption is atomic under contention",
          "[inspect][control][carrier][admission][security][thread]") {
    ControlConnectionAdmissionStore store;
    const auto issued = store.issue(peer(ControlPeerRole::StandaloneHost), host());
    REQUIRE(issued.ticket);

    std::atomic<bool> start{false};
    auto consume = [&] {
        while (!start.load(std::memory_order_acquire)) // unbounded-wait: allow published before get
            std::this_thread::yield();
        return bool(store.consume(issued.ticket->admission_id));
    };
    auto first = std::async(std::launch::async, consume);
    auto second = std::async(std::launch::async, consume);
    start.store(true, std::memory_order_release);
    CHECK(static_cast<int>(first.get()) + static_cast<int>(second.get()) == 1);
    CHECK(store.size() == 0);
}
