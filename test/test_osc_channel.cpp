#include <catch2/catch_test_macros.hpp>

#include <pulp/osc/osc_channel.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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

template <typename Send, typename Pred>
bool send_until(Send send, Pred pred, std::chrono::milliseconds budget = 5s) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        if (!send()) return false;
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

struct PortPair {
    std::uint16_t first = 0;
    std::uint16_t second = 0;
};

std::optional<PortPair> reserve_udp_port_pair() {
    Receiver first;
    Receiver second;
    const auto noop = [](const Message&) {};

    if (!first.listen(0, noop)) return std::nullopt;
    if (!second.listen(0, noop)) {
        first.stop();
        return std::nullopt;
    }

    const PortPair ports{first.local_port(), second.local_port()};
    first.stop();
    second.stop();

    if (ports.first == 0 || ports.second == 0 || ports.first == ports.second) {
        return std::nullopt;
    }
    return ports;
}

struct ChannelPair {
    std::unique_ptr<OscChannel> first;
    std::unique_ptr<OscChannel> second;

    explicit operator bool() const { return first && second; }
};

ChannelPair open_loopback_pair(OscChannelOptions first_options = {},
                               OscChannelOptions second_options = {}) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto ports = reserve_udp_port_pair();
        if (!ports) continue;

        auto first = OscChannel::open("127.0.0.1", ports->second, ports->first,
                                      first_options);
        auto second = OscChannel::open("127.0.0.1", ports->first, ports->second,
                                       second_options);
        if (first && second) {
            return {std::move(first), std::move(second)};
        }
        if (first) first->close();
        if (second) second->close();
    }

    return {};
}

std::unique_ptr<OscChannel> open_loopback_endpoint(OscChannelOptions options = {}) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto ports = reserve_udp_port_pair();
        if (!ports) continue;

        auto channel = OscChannel::open("127.0.0.1", ports->second, ports->first,
                                        options);
        if (channel) return channel;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("OscChannel round-trips an OSC message over UDP loopback", "[osc_channel]") {
    auto pair = open_loopback_pair();
    if (!pair) {
        SUCCEED("could not open loopback UDP pair; skipping");
        return;
    }
    auto& a = pair.first;
    auto& b = pair.second;

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

    REQUIRE(send_until([&] { return a->send(msg); }, [&] {
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
    auto a = open_loopback_endpoint();
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
    auto a = open_loopback_endpoint();
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
    auto a = open_loopback_endpoint();
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

    auto a = open_loopback_endpoint(options);
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
    auto pair = open_loopback_pair();
    if (!pair) {
        SUCCEED("could not open loopback UDP pair; skipping");
        return;
    }
    auto& a = pair.first;
    auto& b = pair.second;

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

    REQUIRE(send_until([&] { return a->send(encoded.data(), encoded.size()); }, [&] {
        std::lock_guard<std::mutex> lock(mu);
        return !received.empty();
    }));

    {
        std::lock_guard<std::mutex> lock(mu);
        // Re-decode on this end and verify every field survives the trip.
        auto decoded = decode(received.data(), received.size());
        REQUIRE(decoded.address == "/raw/path");
        REQUIRE(decoded.get_int(0) == 7);
        REQUIRE(decoded.get_string(1) == "abc");
    }

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

    auto pair = open_loopback_pair({}, options);
    if (!pair) {
        SUCCEED("could not open loopback UDP pair; skipping");
        return;
    }
    auto& a = pair.first;
    auto& b = pair.second;

    std::vector<uint8_t> received;
    b->on_message([&](const pulp::runtime::Message& m) {
        received = m.payload;
    });

    Message msg("/queued");
    msg.add(int32_t{9});
    const auto encoded = encode(msg);

    REQUIRE(send_until([&] { return a->send(encoded.data(), encoded.size()); }, [&] {
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
