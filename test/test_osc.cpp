#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/osc/osc.hpp>
#include <thread>
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

using namespace pulp::osc;
using Catch::Matchers::WithinAbs;

// ── Message ──────────────────────────────────────────────────────────────────

TEST_CASE("OSC Message construction", "[osc][message]") {
    Message msg("/synth/freq");
    msg.add(440.0f).add(42).add(std::string("hello"));

    REQUIRE(msg.address == "/synth/freq");
    REQUIRE(msg.args.size() == 3);
    REQUIRE_THAT(msg.get_float(0), WithinAbs(440.0, 0.01));
    REQUIRE(msg.get_int(1) == 42);
    REQUIRE(msg.get_string(2) == "hello");
}

TEST_CASE("OSC Message get with defaults", "[osc][message]") {
    Message msg("/test");
    msg.add(10);

    REQUIRE(msg.get_int(0) == 10);
    REQUIRE(msg.get_int(5, -1) == -1);      // Out of range
    REQUIRE(msg.get_float(0, 99.0f) == 99.0f); // Wrong type
    REQUIRE(msg.get_string(0, "nope") == "nope");
}

// ── Encoding/Decoding ────────────────────────────────────────────────────────

TEST_CASE("OSC encode/decode round-trip int+float", "[osc][codec]") {
    Message msg("/gain");
    msg.add(-12).add(0.75f);

    auto data = encode(msg);
    REQUIRE(!data.empty());

    auto decoded = decode(data.data(), data.size());
    REQUIRE(decoded.address == "/gain");
    REQUIRE(decoded.args.size() == 2);
    REQUIRE(decoded.get_int(0) == -12);
    REQUIRE_THAT(decoded.get_float(1), WithinAbs(0.75, 0.001));
}

TEST_CASE("OSC encode/decode round-trip string", "[osc][codec]") {
    Message msg("/label");
    msg.add(std::string("Pulp Framework"));

    auto data = encode(msg);
    auto decoded = decode(data.data(), data.size());

    REQUIRE(decoded.address == "/label");
    REQUIRE(decoded.get_string(0) == "Pulp Framework");
}

TEST_CASE("OSC encode/decode round-trip blob", "[osc][codec]") {
    Message msg("/data");
    std::vector<uint8_t> blob = {0xDE, 0xAD, 0xBE, 0xEF};
    msg.add(blob);

    auto data = encode(msg);
    auto decoded = decode(data.data(), data.size());

    REQUIRE(decoded.address == "/data");
    REQUIRE(decoded.args.size() == 1);
    auto& result = std::get<std::vector<uint8_t>>(decoded.args[0]);
    REQUIRE(result.size() == 4);
    REQUIRE(result[0] == 0xDE);
    REQUIRE(result[3] == 0xEF);
}

TEST_CASE("OSC encode/decode mixed types", "[osc][codec]") {
    Message msg("/mix");
    msg.add(1).add(2.5f).add(std::string("test")).add(3);

    auto data = encode(msg);
    auto decoded = decode(data.data(), data.size());

    REQUIRE(decoded.address == "/mix");
    REQUIRE(decoded.args.size() == 4);
    REQUIRE(decoded.get_int(0) == 1);
    REQUIRE_THAT(decoded.get_float(1), WithinAbs(2.5, 0.001));
    REQUIRE(decoded.get_string(2) == "test");
    REQUIRE(decoded.get_int(3) == 3);
}

TEST_CASE("OSC decode invalid data", "[osc][codec]") {
    uint8_t bad[] = {0x00, 0x01, 0x02};
    auto msg = decode(bad, 3);
    REQUIRE(msg.address.empty());
}

TEST_CASE("OSC 4-byte alignment", "[osc][codec]") {
    // Short address "/a" should still produce valid encoding
    Message msg("/a");
    msg.add(42);

    auto data = encode(msg);
    REQUIRE(data.size() % 4 == 0); // All OSC data is 4-byte aligned

    auto decoded = decode(data.data(), data.size());
    REQUIRE(decoded.address == "/a");
    REQUIRE(decoded.get_int(0) == 42);
}

// ── UDP sender/receiver ─────────────────────────────────────────────────────

TEST_CASE("OSC sender/receiver loopback", "[osc][udp]") {
    Message received_msg;
    std::mutex received_msg_mutex;
    std::atomic<bool> got_message{false};

    Receiver rx;
    bool listening = rx.listen(9876, [&](const Message& msg) {
        {
            std::lock_guard<std::mutex> lock(received_msg_mutex);
            received_msg = msg;
        }
        got_message.store(true, std::memory_order_release);
    });
    REQUIRE(listening);
    REQUIRE(rx.is_listening());

    // Give receiver time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", 9876));
    REQUIRE(tx.is_connected());

    Message msg("/test/ping");
    msg.add(42).add(3.14f);
    REQUIRE(tx.send(msg));

    // Wait for message
    for (int i = 0; i < 20 && !got_message.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    REQUIRE(got_message.load(std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(received_msg_mutex);
        REQUIRE(received_msg.address == "/test/ping");
        REQUIRE(received_msg.get_int(0) == 42);
        REQUIRE_THAT(received_msg.get_float(1), WithinAbs(3.14, 0.01));
    }

    tx.disconnect();
    rx.stop();
    REQUIRE_FALSE(tx.is_connected());
    REQUIRE_FALSE(rx.is_listening());
}

// ── Codec edges (spec compliance & decoder robustness) ─────────────────────

TEST_CASE("OSC decode of empty payload yields empty address", "[osc][codec][decode-edge]") {
    auto msg = decode(nullptr, 0);
    REQUIRE(msg.address.empty());
    REQUIRE(msg.args.empty());
}

TEST_CASE("OSC decode of address without leading slash is rejected", "[osc][codec][decode-edge]") {
    Message raw;
    raw.address = "no-slash";
    // bypass add() so we can test the decoder contract directly
    auto data = encode(raw);
    auto decoded = decode(data.data(), data.size());
    REQUIRE(decoded.address.empty());
}

TEST_CASE("OSC decode with no type tag section returns no-arg message",
          "[osc][codec][decode-edge]") {
    // Address alone (no ",," section): valid per OSC 1.0 — produce a
    // zero-arg message, don't blow up.
    std::vector<uint8_t> buf;
    const char addr[] = "/hello";
    buf.insert(buf.end(), addr, addr + sizeof(addr));       // "/hello\0"
    while (buf.size() % 4 != 0) buf.push_back(0);

    auto decoded = decode(buf.data(), buf.size());
    REQUIRE(decoded.address == "/hello");
    REQUIRE(decoded.args.empty());
}

TEST_CASE("OSC decode skips unknown type tags without crashing",
          "[osc][codec][decode-edge]") {
    // Build an ",iZ" tag manually — 'Z' is not defined. Decoder must not
    // read garbage args beyond what it understands.
    std::vector<uint8_t> buf;
    const char addr[] = "/a";
    buf.insert(buf.end(), addr, addr + sizeof(addr));
    while (buf.size() % 4 != 0) buf.push_back(0);
    const char tags[] = ",iZ";
    buf.insert(buf.end(), tags, tags + sizeof(tags));
    while (buf.size() % 4 != 0) buf.push_back(0);
    // int 42
    uint8_t i32[4] = {0, 0, 0, 42};
    buf.insert(buf.end(), i32, i32 + 4);

    auto decoded = decode(buf.data(), buf.size());
    REQUIRE(decoded.address == "/a");
    // Only the 'i' arg landed; 'Z' was skipped rather than corrupting args.
    REQUIRE(decoded.args.size() == 1);
    REQUIRE(decoded.get_int(0) == 42);
}

TEST_CASE("OSC decode of truncated int argument returns the int-typed default",
          "[osc][codec][decode-edge]") {
    Message msg("/t");
    msg.add(0x01020304);
    auto data = encode(msg);
    REQUIRE(data.size() >= 4);
    // Lop off the int bytes so the int tag points past the buffer end.
    auto truncated = std::vector<uint8_t>(data.begin(), data.end() - 4);
    auto decoded = decode(truncated.data(), truncated.size());
    REQUIRE(decoded.address == "/t");
    REQUIRE(decoded.args.size() == 1);
    // read_int32 bounds-checks and returns 0 when the span is short.
    REQUIRE(decoded.get_int(0) == 0);
}

TEST_CASE("OSC decode of truncated blob payload yields empty blob",
          "[osc][codec][decode-edge]") {
    Message msg("/b");
    msg.add(std::vector<uint8_t>{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF});
    auto data = encode(msg);
    // Chop off the trailing blob bytes so the prefixed size walks off the end.
    auto truncated = std::vector<uint8_t>(data.begin(), data.begin() + (data.size() - 4));
    auto decoded = decode(truncated.data(), truncated.size());
    REQUIRE(decoded.address == "/b");
    REQUIRE(decoded.args.size() == 1);
    auto& blob = std::get<std::vector<uint8_t>>(decoded.args[0]);
    REQUIRE(blob.empty());
}

TEST_CASE("OSC encode/decode of empty blob preserves emptiness",
          "[osc][codec][blob]") {
    Message msg("/empty-blob");
    msg.add(std::vector<uint8_t>{});
    auto data = encode(msg);
    REQUIRE(data.size() % 4 == 0);

    auto decoded = decode(data.data(), data.size());
    REQUIRE(decoded.args.size() == 1);
    auto& blob = std::get<std::vector<uint8_t>>(decoded.args[0]);
    REQUIRE(blob.empty());
}

TEST_CASE("OSC encode is deterministic for identical messages",
          "[osc][codec][determinism]") {
    auto make = [] {
        Message msg("/gain/channel/1");
        msg.add(1).add(2.5f).add(std::string("ok")).add(std::vector<uint8_t>{0xDE, 0xAD});
        return encode(msg);
    };
    auto a = make();
    auto b = make();
    REQUIRE(a == b);
}

TEST_CASE("OSC encode output stays 4-byte aligned for odd-length strings",
          "[osc][codec][alignment]") {
    // Address length 5 ("/a/bc") + null = 6, padded to 8.
    // String arg length 7 ("pulp!!") + null = 8, padded to 8.
    Message msg("/a/bc");
    msg.add(std::string("pulp!!!"));
    auto data = encode(msg);
    REQUIRE(data.size() % 4 == 0);
    auto decoded = decode(data.data(), data.size());
    REQUIRE(decoded.address == "/a/bc");
    REQUIRE(decoded.get_string(0) == "pulp!!!");
}

TEST_CASE("OSC encode/decode preserves int32 extremes", "[osc][codec][extremes]") {
    Message msg("/x");
    msg.add(std::numeric_limits<int32_t>::min())
       .add(std::numeric_limits<int32_t>::max())
       .add(0);
    auto data = encode(msg);
    auto decoded = decode(data.data(), data.size());
    REQUIRE(decoded.get_int(0) == std::numeric_limits<int32_t>::min());
    REQUIRE(decoded.get_int(1) == std::numeric_limits<int32_t>::max());
    REQUIRE(decoded.get_int(2) == 0);
}

TEST_CASE("OSC encode/decode preserves float infinities and NaN bit pattern",
          "[osc][codec][extremes]") {
    float nan_in;
    uint32_t nan_bits = 0x7FC00001;  // distinctive quiet NaN payload
    std::memcpy(&nan_in, &nan_bits, 4);

    Message msg("/f");
    msg.add(std::numeric_limits<float>::infinity())
       .add(-std::numeric_limits<float>::infinity())
       .add(nan_in);

    auto data = encode(msg);
    auto decoded = decode(data.data(), data.size());
    REQUIRE(decoded.get_float(0) == std::numeric_limits<float>::infinity());
    REQUIRE(decoded.get_float(1) == -std::numeric_limits<float>::infinity());

    float nan_out = decoded.get_float(2);
    REQUIRE(std::isnan(nan_out));
    uint32_t nan_out_bits;
    std::memcpy(&nan_out_bits, &nan_out, 4);
    REQUIRE(nan_out_bits == nan_bits);
}

TEST_CASE("OSC Sender::send without connect is rejected", "[osc][udp][sender]") {
    Sender tx;
    REQUIRE_FALSE(tx.is_connected());
    Message msg("/x");
    msg.add(1);
    REQUIRE_FALSE(tx.send(msg));
}
