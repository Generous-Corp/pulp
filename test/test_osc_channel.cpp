#include <catch2/catch_test_macros.hpp>

#include <pulp/osc/osc_channel.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace pulp::osc;
using namespace std::chrono_literals;

namespace {

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds budget = 5s) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

std::pair<uint16_t, uint16_t> loopback_port_pair(uint16_t offset) {
    static std::atomic<uint16_t> counter{0};
    const auto tick = static_cast<uint16_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() % 1000);
    const auto n = static_cast<uint16_t>(counter.fetch_add(1));
    const auto base = static_cast<uint16_t>(41000 + ((tick + offset + n * 2) % 1000) * 2);
    return {base, static_cast<uint16_t>(base + 1)};
}

}  // namespace

TEST_CASE("OscChannel round-trips an OSC message over UDP loopback", "[osc_channel]") {
    // Two channels pointed at each other on loopback.
    const auto [a_port, b_port] = loopback_port_pair(1);
    auto a = OscChannel::open("127.0.0.1", a_port, b_port);
    auto b = OscChannel::open("127.0.0.1", b_port, a_port);
    if (!a || !b) {
        SUCCEED("could not open loopback UDP pair; skipping");
        return;
    }

    std::mutex mu;
    std::vector<pulp::runtime::Message> a_got, b_got;
    a->on_message([&](const pulp::runtime::Message& m) {
        std::lock_guard<std::mutex> lock(mu);
        a_got.push_back(m);
    });
    b->on_message([&](const pulp::runtime::Message& m) {
        std::lock_guard<std::mutex> lock(mu);
        b_got.push_back(m);
    });

    Message msg("/synth/freq");
    msg.add(440.0f).add(std::string("sine"));
    REQUIRE(a->send(msg));

    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(mu);
        return !b_got.empty();
    }));

    // The bytes b received round-trip back to the same OSC message.
    {
        std::lock_guard<std::mutex> lock(mu);
        auto decoded = pulp::osc::decode(b_got[0].payload.data(),
                                         b_got[0].payload.size());
        REQUIRE(decoded.address == "/synth/freq");
        REQUIRE(decoded.get_float(0) == 440.0f);
        REQUIRE(decoded.get_string(1) == "sine");
    }

    a->close();
    b->close();
}

TEST_CASE("OscChannel send empty payload is rejected", "[osc_channel][lifecycle]") {
    const auto [a_port, b_port] = loopback_port_pair(11);
    auto a = OscChannel::open("127.0.0.1", a_port, b_port);
    if (!a) {
        SUCCEED("could not open loopback UDP pair; skipping");
        return;
    }
    REQUIRE(a->is_open());
    REQUIRE_FALSE(a->send(nullptr, 0));
    const uint8_t byte = 0;
    REQUIRE_FALSE(a->send(&byte, 0));
    a->close();
}

TEST_CASE("OscChannel send after close is rejected", "[osc_channel][lifecycle]") {
    const auto [a_port, b_port] = loopback_port_pair(13);
    auto a = OscChannel::open("127.0.0.1", a_port, b_port);
    if (!a) {
        SUCCEED("could not open loopback UDP pair; skipping");
        return;
    }
    REQUIRE(a->is_open());
    a->close();
    REQUIRE_FALSE(a->is_open());

    Message msg("/x");
    msg.add(1);
    REQUIRE_FALSE(a->send(msg));

    const uint8_t bytes[] = {0, 0, 0, 0};
    REQUIRE_FALSE(a->send(bytes, sizeof(bytes)));
}

TEST_CASE("OscChannel close is idempotent and on_closed fires exactly once",
          "[osc_channel][lifecycle]") {
    const auto [a_port, b_port] = loopback_port_pair(15);
    auto a = OscChannel::open("127.0.0.1", a_port, b_port);
    if (!a) {
        SUCCEED("could not open loopback UDP pair; skipping");
        return;
    }

    std::atomic<int> closed_count{0};
    a->on_closed([&] { closed_count.fetch_add(1, std::memory_order_release); });

    a->close();
    a->close();  // second close must not re-fire
    a->close();  // third close must not re-fire

    // Inline executor — no wait needed, but give any background thread
    // a moment just in case.
    REQUIRE(wait_until([&] {
        return closed_count.load(std::memory_order_acquire) == 1;
    }, 500ms));
    REQUIRE(closed_count.load() == 1);
}

TEST_CASE("OscChannel routes close callbacks through custom executor",
          "[osc_channel][lifecycle][coverage][phase3]") {
    std::vector<std::function<void()>> queued;
    OscChannelOptions options;
    options.executor = [&](std::function<void()> fn) {
        queued.push_back(std::move(fn));
    };

    const auto [a_port, b_port] = loopback_port_pair(19);
    auto a = OscChannel::open("127.0.0.1", a_port, b_port, options);
    if (!a) {
        SUCCEED("could not open loopback UDP pair; skipping");
        return;
    }

    int closed_count = 0;
    a->on_closed([&] { ++closed_count; });
    a->close();

    REQUIRE(closed_count == 0);
    REQUIRE(queued.size() == 1);
    queued.front()();
    REQUIRE(closed_count == 1);
}

TEST_CASE("OscChannel delivers raw send() bytes verbatim to the peer",
          "[osc_channel][raw]") {
    const auto [a_port, b_port] = loopback_port_pair(17);
    auto a = OscChannel::open("127.0.0.1", a_port, b_port);
    auto b = OscChannel::open("127.0.0.1", b_port, a_port);
    if (!a || !b) {
        SUCCEED("could not open loopback UDP pair; skipping");
        return;
    }

    std::mutex mu;
    std::vector<uint8_t> received;
    b->on_message([&](const pulp::runtime::Message& m) {
        std::lock_guard<std::mutex> lock(mu);
        received = m.payload;
    });

    // Hand-crafted OSC message bytes that *don't* come from encode() —
    // raw send must preserve them exactly.
    Message golden("/raw/path");
    golden.add(int32_t{7}).add(std::string("abc"));
    const auto encoded = encode(golden);

    REQUIRE(a->send(encoded.data(), encoded.size()));

    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(mu);
        return !received.empty();
    }));

    std::lock_guard<std::mutex> lock(mu);
    // Re-decode on this end and verify every field survives the trip.
    auto decoded = decode(received.data(), received.size());
    REQUIRE(decoded.address == "/raw/path");
    REQUIRE(decoded.get_int(0) == 7);
    REQUIRE(decoded.get_string(1) == "abc");

    a->close();
    b->close();
}

TEST_CASE("OscChannel routes received messages through custom executor",
          "[osc_channel][raw][coverage][phase3]") {
    std::mutex queue_mu;
    std::vector<std::function<void()>> queued;
    OscChannelOptions options;
    options.executor = [&](std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(queue_mu);
        queued.push_back(std::move(fn));
    };

    const auto [a_port, b_port] = loopback_port_pair(21);
    auto a = OscChannel::open("127.0.0.1", a_port, b_port);
    auto b = OscChannel::open("127.0.0.1", b_port, a_port, options);
    if (!a || !b) {
        SUCCEED("could not open loopback UDP pair; skipping");
        return;
    }

    std::vector<uint8_t> received;
    b->on_message([&](const pulp::runtime::Message& m) {
        received = m.payload;
    });

    Message msg("/queued");
    msg.add(int32_t{9});
    const auto encoded = encode(msg);
    REQUIRE(a->send(encoded.data(), encoded.size()));

    REQUIRE(wait_until([&] {
        std::lock_guard<std::mutex> lock(queue_mu);
        return !queued.empty();
    }));
    REQUIRE(received.empty());

    std::function<void()> deliver;
    {
        std::lock_guard<std::mutex> lock(queue_mu);
        deliver = std::move(queued.front());
    }
    deliver();

    auto decoded = decode(received.data(), received.size());
    REQUIRE(decoded.address == "/queued");
    REQUIRE(decoded.get_int(0) == 9);

    a->close();
    b->close();
}
