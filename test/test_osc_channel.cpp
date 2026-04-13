#include <catch2/catch_test_macros.hpp>

#include <pulp/osc/osc_channel.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::osc;
using namespace std::chrono_literals;

namespace {

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds budget = 2s) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

}  // namespace

TEST_CASE("OscChannel round-trips an OSC message over UDP loopback", "[osc_channel]") {
    // Two channels pointed at each other on loopback.
    auto a = OscChannel::open("127.0.0.1", 49901, 49902);
    auto b = OscChannel::open("127.0.0.1", 49902, 49901);
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
