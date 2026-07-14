#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <pulp/osc/bundle.hpp>
#include <pulp/osc/osc.hpp>
#include <thread>
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <variant>
#include <vector>

using namespace pulp::osc;
using Catch::Matchers::WithinAbs;

namespace {

constexpr uint16_t kOscHostnameTestPort = 29876;

#if defined(_WIN32)
using TestSocketHandle = SOCKET;
using TestSockLen = int;
constexpr TestSocketHandle kInvalidTestSocket = INVALID_SOCKET;

struct TestWinsockInit {
    bool ok = false;

    TestWinsockInit() {
        WSADATA wsa{};
        ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }

    ~TestWinsockInit() {
        if (ok) WSACleanup();
    }
};

bool ensure_test_winsock() {
    static TestWinsockInit init;
    return init.ok;
}

void close_test_socket(TestSocketHandle sock) {
    closesocket(sock);
}
#else
using TestSocketHandle = int;
using TestSockLen = socklen_t;
constexpr TestSocketHandle kInvalidTestSocket = -1;

bool ensure_test_winsock() {
    return true;
}

void close_test_socket(TestSocketHandle sock) {
    close(sock);
}
#endif

void append_osc_string(std::vector<uint8_t>& data, std::string_view text) {
    data.insert(data.end(), text.begin(), text.end());
    data.push_back(0);
    while (data.size() % 4 != 0)
        data.push_back(0);
}

bool send_empty_udp_datagram(uint16_t port) {
    if (!ensure_test_winsock()) return false;
    auto sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == kInvalidTestSocket) return false;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr) != 1) {
        close_test_socket(sock);
        return false;
    }

    const char payload[] = "";
    const auto sent = sendto(sock,
                             payload,
                             0,
                             0,
                             reinterpret_cast<sockaddr*>(&dest),
                             static_cast<TestSockLen>(sizeof(dest)));
    close_test_socket(sock);
    return sent == 0;
}

} // namespace

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

TEST_CASE("OSC Message typed defaults cover every wrong-type accessor", "[osc][message][issue-644]") {
    Message msg("/defaults");
    msg.add(std::string("label"));
    msg.add(std::vector<uint8_t>{0x01, 0x02});
    msg.add(1.5f);

    REQUIRE(msg.get_int(0, -10) == -10);
    REQUIRE(msg.get_float(1, -2.0f) == -2.0f);
    REQUIRE(msg.get_string(2, "fallback") == "fallback");
    REQUIRE(msg.get_string(99, "missing") == "missing");
}

TEST_CASE("OSC Message defaults cover empty and wrong-type accessors",
          "[osc][message]") {
    Message msg;
    REQUIRE(msg.get_int(0, -7) == -7);
    REQUIRE(msg.get_float(0, 2.5f) == 2.5f);
    REQUIRE(msg.get_string(0, "missing") == "missing");

    msg.add(1.0f)
       .add(std::string("label"))
       .add(std::vector<uint8_t>{0x01, 0x02});
    REQUIRE(msg.get_int(0, 9) == 9);
    REQUIRE(msg.get_float(1, 3.0f) == 3.0f);
    REQUIRE(msg.get_string(2, "blob") == "blob");
}

TEST_CASE("OSC Message add overloads preserve fluent chaining and argument types",
          "[osc][message]") {
    Message msg("/chain");
    auto* returned = &msg.add(7)
        .add(0.5f)
        .add(std::string("name"))
        .add(std::vector<uint8_t>{0x10, 0x20, 0x30});

    REQUIRE(returned == &msg);
    REQUIRE(msg.args.size() == 4);
    REQUIRE(std::holds_alternative<int32_t>(msg.args[0]));
    REQUIRE(std::holds_alternative<float>(msg.args[1]));
    REQUIRE(std::holds_alternative<std::string>(msg.args[2]));
    REQUIRE(std::holds_alternative<std::vector<uint8_t>>(msg.args[3]));
    REQUIRE(std::get<std::vector<uint8_t>>(msg.args[3]).back() == 0x30);
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

TEST_CASE("OSC encode with no arguments emits an empty type-tag section",
          "[osc][codec]") {
    Message msg("/plain");

    auto data = encode(msg);
    auto decoded = decode(data.data(), data.size());

    REQUIRE(data.size() == 12);
    REQUIRE(decoded.address == "/plain");
    REQUIRE(decoded.args.empty());
    REQUIRE(data[8] == ',');
    REQUIRE(data[9] == 0);
}

TEST_CASE("OSC decode accepts explicit comma tag with no arguments",
          "[osc][codec]") {
    std::vector<uint8_t> data;
    append_osc_string(data, "/empty-tags");
    append_osc_string(data, ",");

    auto decoded = decode(data.data(), data.size());

    REQUIRE(decoded.address == "/empty-tags");
    REQUIRE(decoded.args.empty());
}

TEST_CASE("OSC decode ignores unknown type tags without adding arguments",
          "[osc][codec]") {
    std::vector<uint8_t> data;
    append_osc_string(data, "/unknown-tags");
    append_osc_string(data, ",TFNz");

    auto decoded = decode(data.data(), data.size());

    REQUIRE(decoded.address == "/unknown-tags");
    REQUIRE(decoded.args.empty());
}

TEST_CASE("OSC decode skips unknown tags without consuming later argument bytes",
          "[osc][codec]") {
    std::vector<uint8_t> data;
    append_osc_string(data, "/mixed");
    append_osc_string(data, ",xi");
    data.insert(data.end(), {0x00, 0x00, 0x00, 0x2A});

    auto decoded = decode(data.data(), data.size());
    REQUIRE(decoded.address == "/mixed");
    REQUIRE(decoded.args.size() == 1);
    REQUIRE(decoded.get_int(0) == 42);
}

TEST_CASE("OSC decode rejects truncated typed payloads as malformed",
          "[osc][codec]") {
    // A type tag string (",ifsb") with a truncated payload is malformed: the
    // decoder must fail closed to the empty-address sentinel rather than
    // fabricate 0/default args that would dispatch a real control change. The
    // receiver and bundle paths already treat empty-address as malformed.
    std::vector<uint8_t> data;
    append_osc_string(data, "/truncated");
    append_osc_string(data, ",ifsb");
    data.insert(data.end(), {0x00, 0x00});  // partial int, then no remaining payload

    auto decoded = decode(data.data(), data.size());

    REQUIRE(decoded.address.empty());
    REQUIRE(decoded.args.empty());
}

TEST_CASE("OSC decode handles non-null-terminated bounded address payload",
          "[osc][codec]") {
    const std::vector<uint8_t> data{'/', 'b', 'a', 'r'};

    auto decoded = decode(data.data(), data.size());

    REQUIRE(decoded.address == "/bar");
    REQUIRE(decoded.args.empty());
}

TEST_CASE("OSC encode/decode preserves blob payloads at padding boundaries",
          "[osc][codec]") {
    Message msg("/blob-padding");
    msg.add(std::vector<uint8_t>{0x01, 0x02, 0x03});
    msg.add(std::vector<uint8_t>{0x04, 0x05, 0x06, 0x07});

    auto data = encode(msg);
    auto decoded = decode(data.data(), data.size());

    REQUIRE(decoded.address == "/blob-padding");
    REQUIRE(decoded.args.size() == 2);
    REQUIRE(std::get<std::vector<uint8_t>>(decoded.args[0])
            == std::vector<uint8_t>{0x01, 0x02, 0x03});
    REQUIRE(std::get<std::vector<uint8_t>>(decoded.args[1])
            == std::vector<uint8_t>{0x04, 0x05, 0x06, 0x07});
}

TEST_CASE("OSC decode preserves empty blob alignment before following arguments",
          "[osc][codec][blob]") {
    Message msg("/zero-blob-next");
    msg.add(std::vector<uint8_t>{});
    msg.add(77);

    auto data = encode(msg);
    auto decoded = decode(data.data(), data.size());

    REQUIRE(decoded.address == "/zero-blob-next");
    REQUIRE(decoded.args.size() == 2);
    REQUIRE(std::get<std::vector<uint8_t>>(decoded.args[0]).empty());
    REQUIRE(decoded.get_int(1) == 77);
}

// ── UDP sender/receiver ─────────────────────────────────────────────────────

TEST_CASE("OSC sender/receiver loopback", "[osc][udp]") {
    Message received_msg;
    std::mutex received_msg_mutex;
    std::atomic<bool> got_message{false};

    Receiver rx;
    bool listening = rx.listen(0, [&](const Message& msg) {
        {
            std::lock_guard<std::mutex> lock(received_msg_mutex);
            received_msg = msg;
        }
        got_message.store(true, std::memory_order_release);
    });
    REQUIRE(listening);
    REQUIRE(rx.is_listening());
    const auto port = rx.local_port();
    REQUIRE(port != 0);

    // Give receiver time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", port));
    REQUIRE(tx.is_connected());

    Message msg("/test/ping");
    msg.add(42).add(3.14f);

    // UDP delivery is best-effort even on loopback; retry within a
    // bounded window so scheduler/load hiccups do not make the test flaky.
    for (int i = 0; i < 100 && !got_message.load(std::memory_order_acquire); ++i) {
        REQUIRE(tx.send(msg));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
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
    REQUIRE(rx.local_port() == 0);
}

TEST_CASE("OSC Sender resolves localhost hostnames", "[osc][udp][sender]") {
    Sender tx;
    REQUIRE(tx.connect("localhost", kOscHostnameTestPort));
    REQUIRE(tx.is_connected());

    uint8_t byte = 0;
    REQUIRE_FALSE(tx.send_raw(nullptr, 1));
    REQUIRE_FALSE(tx.send_raw(&byte, 0));

    tx.disconnect();
    REQUIRE_FALSE(tx.is_connected());
}

TEST_CASE("OSC Sender rejects invalid hostnames", "[osc][udp][sender]") {
    Sender tx;
    REQUIRE_FALSE(tx.connect("999.999.999.999", kOscHostnameTestPort));
    REQUIRE_FALSE(tx.is_connected());
    tx.disconnect();
    REQUIRE_FALSE(tx.is_connected());
}

TEST_CASE("OSC Sender failed reconnect preserves the existing destination",
          "[osc][udp][sender]") {
    std::atomic<int> handled{0};

    Receiver rx;
    REQUIRE(rx.listen(0, [&](const Message& msg) {
        if (msg.address == "/sender/reconnect-fail" && msg.get_int(0) == 9) {
            handled.fetch_add(1, std::memory_order_release);
        }
    }));
    const auto port = rx.local_port();
    REQUIRE(port != 0);

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", port));
    REQUIRE(tx.is_connected());
    REQUIRE_FALSE(tx.connect("999.999.999.999", kOscHostnameTestPort));
    REQUIRE(tx.is_connected());

    Message msg("/sender/reconnect-fail");
    msg.add(9);
    for (int i = 0; i < 100 && handled.load(std::memory_order_acquire) == 0; ++i) {
        REQUIRE(tx.send(msg));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(handled.load(std::memory_order_acquire) > 0);
    tx.disconnect();
    rx.stop();
}

TEST_CASE("OSC Sender successful reconnect replaces the existing destination",
          "[osc][udp][sender]") {
    std::atomic<int> first_receiver{0};
    std::atomic<int> second_receiver{0};

    Receiver rx1;
    REQUIRE(rx1.listen(0, [&](const Message& msg) {
        if (msg.address == "/sender/reconnect-success/first")
            first_receiver.fetch_add(1, std::memory_order_release);
    }));

    Receiver rx2;
    REQUIRE(rx2.listen(0, [&](const Message& msg) {
        if (msg.address == "/sender/reconnect-success/second" && msg.get_int(0) == 22)
            second_receiver.fetch_add(1, std::memory_order_release);
    }));

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", rx1.local_port()));
    REQUIRE(tx.connect("127.0.0.1", rx2.local_port()));
    REQUIRE(tx.is_connected());

    Message first("/sender/reconnect-success/first");
    first.add(11);
    Message second("/sender/reconnect-success/second");
    second.add(22);

    for (int i = 0; i < 100
         && second_receiver.load(std::memory_order_acquire) == 0; ++i) {
        REQUIRE(tx.send(first));
        REQUIRE(tx.send(second));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(second_receiver.load(std::memory_order_acquire) > 0);
    REQUIRE(first_receiver.load(std::memory_order_acquire) == 0);

    tx.disconnect();
    rx1.stop();
    rx2.stop();
}

TEST_CASE("OSC Sender sends caller-owned raw packets over loopback",
          "[osc][udp][sender]") {
    std::atomic<int> handled{0};
    Message received;
    std::mutex received_mutex;

    Receiver rx;
    REQUIRE(rx.listen(0, [&](const Message& msg) {
        std::lock_guard<std::mutex> lock(received_mutex);
        received = msg;
        handled.fetch_add(1, std::memory_order_release);
    }));
    REQUIRE(rx.local_port() != 0);

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", rx.local_port()));

    Message msg("/raw/sender");
    msg.add(321);
    const auto data = encode(msg);
    for (int i = 0; i < 100 && handled.load(std::memory_order_acquire) == 0; ++i) {
        REQUIRE(tx.send_raw(data.data(), data.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(handled.load(std::memory_order_acquire) > 0);
    {
        std::lock_guard<std::mutex> lock(received_mutex);
        REQUIRE(received.address == "/raw/sender");
        REQUIRE(received.get_int(0) == 321);
    }

    tx.disconnect();
    rx.stop();
    REQUIRE_FALSE(tx.is_connected());
    REQUIRE_FALSE(rx.is_listening());
}

TEST_CASE("OSC Receiver routes direct messages through address-pattern listeners",
          "[osc][udp][receiver][pattern]") {
    std::atomic<int> all_messages{0};
    std::atomic<int> mix_messages{0};
    std::atomic<int> note_messages{0};
    std::atomic<int> miss_messages{0};

    ReceiverOptions options;
    options.on_message = [&](const Message&) {
        all_messages.fetch_add(1, std::memory_order_release);
    };
    options.routes.push_back({"/mix/*", [&](const Message& msg) {
        if (msg.address == "/mix/gain")
            mix_messages.fetch_add(msg.get_int(0), std::memory_order_release);
    }});
    options.routes.push_back({"/mix/*", {}});
    options.routes.push_back({"/note/[0-9]", [&](const Message&) {
        note_messages.fetch_add(1, std::memory_order_release);
    }});
    options.routes.push_back({"/unmatched/*", [&](const Message&) {
        miss_messages.fetch_add(1, std::memory_order_release);
    }});

    Receiver rx;
    REQUIRE(rx.listen_with_options(0, std::move(options)));
    const auto port = rx.local_port();
    REQUIRE(port != 0);

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", port));

    Message gain("/mix/gain");
    gain.add(7);
    Message note("/note/4");
    note.add(64);
    Message bypass("/other/path");
    bypass.add(1);

    for (int i = 0; i < 100
         && (mix_messages.load(std::memory_order_acquire) == 0
             || note_messages.load(std::memory_order_acquire) == 0
             || all_messages.load(std::memory_order_acquire) < 3); ++i) {
        REQUIRE(tx.send(gain));
        REQUIRE(tx.send(note));
        REQUIRE(tx.send(bypass));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(mix_messages.load(std::memory_order_acquire) >= 7);
    REQUIRE(note_messages.load(std::memory_order_acquire) > 0);
    REQUIRE(all_messages.load(std::memory_order_acquire) >= 3);
    REQUIRE(miss_messages.load(std::memory_order_acquire) == 0);

    tx.disconnect();
    rx.stop();
}

TEST_CASE("OSC Sender sends typed bundles and Receiver dispatches bundle callbacks",
          "[osc][udp][bundle][receiver]") {
    std::atomic<int> bundle_callbacks{0};
    std::atomic<int> nested_bundle_callbacks{0};
    std::atomic<int> bundle_element_count{0};
    std::atomic<int> message_callbacks{0};
    std::atomic<int> synth_route_sum{0};
    std::atomic<int> nested_route_sum{0};
    std::atomic<int> miss_callbacks{0};

    ReceiverOptions options;
    options.on_bundle = [&](const Bundle& bundle) {
        const auto element_count = static_cast<int>(bundle.elements.size());
        bundle_element_count.store(element_count, std::memory_order_release);
        if (element_count == 1)
            nested_bundle_callbacks.fetch_add(1, std::memory_order_release);
        bundle_callbacks.fetch_add(1, std::memory_order_release);
    };
    options.on_message = [&](const Message&) {
        message_callbacks.fetch_add(1, std::memory_order_release);
    };
    options.routes.push_back({"/synth/*", [&](const Message& msg) {
        synth_route_sum.fetch_add(msg.get_int(0), std::memory_order_release);
    }});
    options.routes.push_back({"/nested/{lead,bass}", [&](const Message& msg) {
        nested_route_sum.fetch_add(msg.get_int(0), std::memory_order_release);
    }});
    options.routes.push_back({"/no/match", [&](const Message&) {
        miss_callbacks.fetch_add(1, std::memory_order_release);
    }});

    Receiver rx;
    REQUIRE(rx.listen_with_options(0, std::move(options)));
    const auto port = rx.local_port();
    REQUIRE(port != 0);

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", port));

    Bundle nested;
    Message nested_msg("/nested/lead");
    nested_msg.add(5);
    nested.add(std::move(nested_msg));

    Bundle bundle;
    Message synth_msg("/synth/gain");
    synth_msg.add(11);
    bundle.add(std::move(synth_msg));
    Message unrelated("/drum/kick");
    unrelated.add(1);
    bundle.add(std::move(unrelated));
    bundle.add(std::move(nested));

    for (int i = 0; i < 100
         && (bundle_callbacks.load(std::memory_order_acquire) == 0
             || synth_route_sum.load(std::memory_order_acquire) == 0
             || nested_route_sum.load(std::memory_order_acquire) == 0
             || message_callbacks.load(std::memory_order_acquire) < 3); ++i) {
        REQUIRE(tx.send(bundle));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(bundle_callbacks.load(std::memory_order_acquire) > 0);
    REQUIRE(nested_bundle_callbacks.load(std::memory_order_acquire) == 0);
    REQUIRE(bundle_element_count.load(std::memory_order_acquire) == 3);
    REQUIRE(message_callbacks.load(std::memory_order_acquire) >= 3);
    REQUIRE(synth_route_sum.load(std::memory_order_acquire) >= 11);
    REQUIRE(nested_route_sum.load(std::memory_order_acquire) >= 5);
    REQUIRE(miss_callbacks.load(std::memory_order_acquire) == 0);

    tx.disconnect();
    rx.stop();
}

TEST_CASE("OSC Sender sends bundles after disconnect and reconnect",
          "[osc][udp][bundle][sender]") {
    std::atomic<int> first_receiver{0};
    std::atomic<int> second_receiver{0};

    Receiver rx1;
    REQUIRE(rx1.listen(0, [&](const Message& msg) {
        if (msg.address == "/bundle/reconnect/first" && msg.get_int(0) == 1)
            first_receiver.fetch_add(1, std::memory_order_release);
    }));

    Receiver rx2;
    REQUIRE(rx2.listen(0, [&](const Message& msg) {
        if (msg.address == "/bundle/reconnect/second" && msg.get_int(0) == 2)
            second_receiver.fetch_add(1, std::memory_order_release);
    }));

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", rx1.local_port()));

    Bundle first_bundle;
    Message first_msg("/bundle/reconnect/first");
    first_msg.add(1);
    first_bundle.add(std::move(first_msg));

    for (int i = 0; i < 100
         && first_receiver.load(std::memory_order_acquire) == 0; ++i) {
        REQUIRE(tx.send(first_bundle));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(first_receiver.load(std::memory_order_acquire) > 0);

    tx.disconnect();
    REQUIRE(tx.connect("127.0.0.1", rx2.local_port()));

    Bundle second_bundle;
    Message second_msg("/bundle/reconnect/second");
    second_msg.add(2);
    second_bundle.add(std::move(second_msg));

    for (int i = 0; i < 100
         && second_receiver.load(std::memory_order_acquire) == 0; ++i) {
        REQUIRE(tx.send(second_bundle));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(second_receiver.load(std::memory_order_acquire) > 0);
    tx.disconnect();
    rx1.stop();
    rx2.stop();
}

TEST_CASE("OSC Receiver dispatches empty bundles without message callbacks",
          "[osc][udp][bundle][receiver]") {
    std::atomic<int> empty_bundles{0};
    std::atomic<int> messages{0};

    ReceiverOptions options;
    options.on_bundle = [&](const Bundle& bundle) {
        if (bundle.elements.empty())
            empty_bundles.fetch_add(1, std::memory_order_release);
    };
    options.on_message = [&](const Message&) {
        messages.fetch_add(1, std::memory_order_release);
    };

    Receiver rx;
    REQUIRE(rx.listen_with_options(0, std::move(options)));
    const auto port = rx.local_port();
    REQUIRE(port != 0);

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", port));

    Bundle bundle;
    for (int i = 0; i < 100
         && empty_bundles.load(std::memory_order_acquire) == 0; ++i) {
        REQUIRE(tx.send(bundle));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(empty_bundles.load(std::memory_order_acquire) > 0);
    REQUIRE(messages.load(std::memory_order_acquire) == 0);
    tx.disconnect();
    rx.stop();
}

TEST_CASE("OSC Receiver accepts bundle datagrams larger than four kilobytes",
          "[osc][udp][bundle][receiver]") {
    std::atomic<int> bundle_callbacks{0};
    std::atomic<int> blob_size{0};
    std::atomic<int> blob_front{0};
    std::atomic<int> blob_back{0};

    ReceiverOptions options;
    options.on_bundle = [&](const Bundle& bundle) {
        if (bundle.elements.size() == 1)
            bundle_callbacks.fetch_add(1, std::memory_order_release);
    };
    options.on_message = [&](const Message& msg) {
        if (msg.address != "/bundle/large" || msg.args.empty()) return;
        const auto* blob = std::get_if<std::vector<uint8_t>>(&msg.args[0]);
        if (blob == nullptr || blob->empty()) return;
        blob_size.store(static_cast<int>(blob->size()), std::memory_order_release);
        blob_front.store(static_cast<int>(blob->front()), std::memory_order_release);
        blob_back.store(static_cast<int>(blob->back()), std::memory_order_release);
    };

    Receiver rx;
    REQUIRE(rx.listen_with_options(0, std::move(options)));
    const auto port = rx.local_port();
    REQUIRE(port != 0);

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", port));

    std::vector<uint8_t> payload(5000);
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<uint8_t>(i & 0xFF);

    Bundle bundle;
    Message msg("/bundle/large");
    msg.add(payload);
    bundle.add(std::move(msg));

    for (int i = 0; i < 100
         && (bundle_callbacks.load(std::memory_order_acquire) == 0
             || blob_size.load(std::memory_order_acquire) == 0); ++i) {
        REQUIRE(tx.send(bundle));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(bundle_callbacks.load(std::memory_order_acquire) > 0);
    REQUIRE(blob_size.load(std::memory_order_acquire)
            == static_cast<int>(payload.size()));
    REQUIRE(blob_front.load(std::memory_order_acquire)
            == static_cast<int>(payload.front()));
    REQUIRE(blob_back.load(std::memory_order_acquire)
            == static_cast<int>(payload.back()));

    tx.disconnect();
    rx.stop();
}

TEST_CASE("OSC Sender disconnects idempotently and reconnects to loopback",
          "[osc][udp][sender]") {
    std::atomic<int> handled{0};

    Receiver rx;
    REQUIRE(rx.listen(0, [&](const Message& msg) {
        if (msg.address == "/sender/reconnect" && msg.get_int(0) == 5) {
            handled.fetch_add(1, std::memory_order_release);
        }
    }));
    const auto port = rx.local_port();
    REQUIRE(port != 0);

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", port));
    REQUIRE(tx.is_connected());

    tx.disconnect();
    REQUIRE_FALSE(tx.is_connected());
    tx.disconnect();

    Message rejected("/sender/reconnect");
    rejected.add(5);
    REQUIRE_FALSE(tx.send(rejected));

    REQUIRE(tx.connect("127.0.0.1", port));
    REQUIRE(tx.is_connected());

    Message msg("/sender/reconnect");
    msg.add(5);
    for (int i = 0; i < 100 && handled.load(std::memory_order_acquire) == 0; ++i) {
        REQUIRE(tx.send(msg));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(handled.load(std::memory_order_acquire) > 0);
    tx.disconnect();
    rx.stop();
    REQUIRE_FALSE(tx.is_connected());
    REQUIRE_FALSE(rx.is_listening());
}

TEST_CASE("OSC Receiver drops invalid datagrams without invoking handler", "[osc][udp][receiver]") {
    std::atomic<int> handled{0};

    Receiver rx;
    REQUIRE(rx.listen(0, [&](const Message&) {
        handled.fetch_add(1, std::memory_order_relaxed);
    }));
    const auto port = rx.local_port();
    REQUIRE(port != 0);

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", port));

    uint8_t invalid[] = {0x00, 0x01, 0x02};
    REQUIRE(tx.send_raw(invalid, sizeof(invalid)));

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    rx.stop();
    tx.disconnect();

    REQUIRE(handled.load(std::memory_order_relaxed) == 0);
    REQUIRE_FALSE(rx.is_listening());
}

TEST_CASE("OSC Receiver reports malformed message and bundle packets",
          "[osc][udp][receiver][error]") {
    std::atomic<int> errors{0};
    std::atomic<int> malformed_messages{0};
    std::atomic<int> malformed_bundles{0};
    std::atomic<int> handled_messages{0};
    std::atomic<int> handled_bundles{0};

    ReceiverOptions options;
    options.on_message = [&](const Message&) {
        handled_messages.fetch_add(1, std::memory_order_release);
    };
    options.on_bundle = [&](const Bundle&) {
        handled_bundles.fetch_add(1, std::memory_order_release);
    };
    options.on_error = [&](std::string_view reason) {
        errors.fetch_add(1, std::memory_order_release);
        if (reason == "malformed OSC message")
            malformed_messages.fetch_add(1, std::memory_order_release);
        if (reason == "malformed OSC bundle")
            malformed_bundles.fetch_add(1, std::memory_order_release);
    };

    Receiver rx;
    REQUIRE(rx.listen_with_options(0, std::move(options)));
    const auto port = rx.local_port();
    REQUIRE(port != 0);

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", port));

    const uint8_t bad_message[] = {0x00, 0x01, 0x02};
    const uint8_t bad_bundle[] = {
        '#', 'b', 'u', 'n', 'd', 'l', 'e', 0,
        0, 0, 0, 0, 0, 0, 0, 1,
        0x12
    };

    for (int i = 0; i < 100
         && (malformed_messages.load(std::memory_order_acquire) == 0
             || malformed_bundles.load(std::memory_order_acquire) == 0); ++i) {
        REQUIRE(tx.send_raw(bad_message, sizeof(bad_message)));
        REQUIRE(tx.send_raw(bad_bundle, sizeof(bad_bundle)));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(errors.load(std::memory_order_acquire) >= 2);
    REQUIRE(malformed_messages.load(std::memory_order_acquire) > 0);
    REQUIRE(malformed_bundles.load(std::memory_order_acquire) > 0);
    REQUIRE(handled_messages.load(std::memory_order_acquire) == 0);
    REQUIRE(handled_bundles.load(std::memory_order_acquire) == 0);

    tx.disconnect();
    rx.stop();
}

TEST_CASE("OSC Receiver reports empty UDP datagrams as malformed messages",
          "[osc][udp][receiver][error]") {
    std::atomic<int> errors{0};
    std::atomic<int> handled_messages{0};

    ReceiverOptions options;
    options.on_message = [&](const Message&) {
        handled_messages.fetch_add(1, std::memory_order_release);
    };
    options.on_error = [&](std::string_view reason) {
        if (reason == "malformed OSC message")
            errors.fetch_add(1, std::memory_order_release);
    };

    Receiver rx;
    REQUIRE(rx.listen_with_options(0, std::move(options)));
    const auto port = rx.local_port();
    REQUIRE(port != 0);

    for (int i = 0; i < 100 && errors.load(std::memory_order_acquire) == 0; ++i) {
        REQUIRE(send_empty_udp_datagram(port));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(errors.load(std::memory_order_acquire) > 0);
    REQUIRE(handled_messages.load(std::memory_order_acquire) == 0);
    rx.stop();
}

TEST_CASE("OSC Receiver callbacks can request stop without self-joining",
          "[osc][udp][receiver][lifecycle]") {
    {
        std::atomic<int> errors{0};
        std::atomic<bool> stop_returned{false};
        std::atomic<int> restarted_messages{0};
        Receiver rx;

        ReceiverOptions options;
        options.on_error = [&](std::string_view) {
            errors.fetch_add(1, std::memory_order_release);
            rx.stop();
            stop_returned.store(true, std::memory_order_release);
        };

        REQUIRE(rx.listen_with_options(0, std::move(options)));
        const auto port = rx.local_port();
        REQUIRE(port != 0);

        Sender tx;
        REQUIRE(tx.connect("127.0.0.1", port));

        const uint8_t bad_message[] = {0x00, 0x01, 0x02};
        for (int i = 0; i < 100 && !stop_returned.load(std::memory_order_acquire); ++i) {
            REQUIRE(tx.send_raw(bad_message, sizeof(bad_message)));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        REQUIRE(errors.load(std::memory_order_acquire) > 0);
        REQUIRE(stop_returned.load(std::memory_order_acquire));
        tx.disconnect();

        Receiver rebound;
        REQUIRE(rebound.listen(port, [](const Message&) {}));
        rebound.stop();

        REQUIRE(rx.listen(0, [&](const Message& msg) {
            if (msg.address == "/callback/restart" && msg.get_int(0) == 17)
                restarted_messages.fetch_add(1, std::memory_order_release);
        }));
        REQUIRE(rx.is_listening());
        REQUIRE(rx.local_port() != 0);

        REQUIRE(tx.connect("127.0.0.1", rx.local_port()));
        Message restart_msg("/callback/restart");
        restart_msg.add(17);
        for (int i = 0; i < 100
             && restarted_messages.load(std::memory_order_acquire) == 0; ++i) {
            REQUIRE(tx.send(restart_msg));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        REQUIRE(restarted_messages.load(std::memory_order_acquire) > 0);
        tx.disconnect();
        rx.stop();
        REQUIRE_FALSE(rx.is_listening());
    }

    {
        std::atomic<int> errors{0};
        std::atomic<int> self_listen_rejected{0};
        Receiver rx;

        ReceiverOptions options;
        options.on_error = [&](std::string_view) {
            errors.fetch_add(1, std::memory_order_release);
            rx.stop();
            if (!rx.listen(0, [](const Message&) {}))
                self_listen_rejected.fetch_add(1, std::memory_order_release);
        };

        REQUIRE(rx.listen_with_options(0, std::move(options)));
        const auto port = rx.local_port();
        REQUIRE(port != 0);

        Sender tx;
        REQUIRE(tx.connect("127.0.0.1", port));

        const uint8_t bad_message[] = {0x00, 0x01, 0x02};
        for (int i = 0; i < 100
             && self_listen_rejected.load(std::memory_order_acquire) == 0; ++i) {
            REQUIRE(tx.send_raw(bad_message, sizeof(bad_message)));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        REQUIRE(errors.load(std::memory_order_acquire) > 0);
        REQUIRE(self_listen_rejected.load(std::memory_order_acquire) > 0);
        tx.disconnect();
        rx.stop();
        REQUIRE_FALSE(rx.is_listening());
    }

    {
        std::atomic<int> bundles{0};
        Receiver rx;

        ReceiverOptions options;
        options.on_bundle = [&](const Bundle&) {
            bundles.fetch_add(1, std::memory_order_release);
            rx.stop();
        };

        REQUIRE(rx.listen_with_options(0, std::move(options)));
        const auto port = rx.local_port();
        REQUIRE(port != 0);

        Sender tx;
        REQUIRE(tx.connect("127.0.0.1", port));

        Bundle bundle;
        Message msg("/callback/stop");
        msg.add(1);
        bundle.add(std::move(msg));

        for (int i = 0; i < 100 && bundles.load(std::memory_order_acquire) == 0; ++i) {
            REQUIRE(tx.send(bundle));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        REQUIRE(bundles.load(std::memory_order_acquire) > 0);
        tx.disconnect();
        rx.stop();
        REQUIRE_FALSE(rx.is_listening());
    }
}

TEST_CASE("OSC Receiver stop short-circuits current datagram callback fanout",
          "[osc][udp][receiver][lifecycle]") {
    {
        std::atomic<int> bundles{0};
        std::atomic<int> messages{0};
        Receiver rx;

        ReceiverOptions options;
        options.on_bundle = [&](const Bundle&) {
            bundles.fetch_add(1, std::memory_order_release);
            rx.stop();
        };
        options.on_message = [&](const Message&) {
            messages.fetch_add(1, std::memory_order_release);
        };

        REQUIRE(rx.listen_with_options(0, std::move(options)));
        const auto port = rx.local_port();
        REQUIRE(port != 0);

        Sender tx;
        REQUIRE(tx.connect("127.0.0.1", port));

        Bundle bundle;
        Message msg("/fanout/bundle");
        msg.add(1);
        bundle.add(std::move(msg));

        for (int i = 0; i < 100 && bundles.load(std::memory_order_acquire) == 0; ++i) {
            REQUIRE(tx.send(bundle));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        REQUIRE(bundles.load(std::memory_order_acquire) > 0);
        REQUIRE(messages.load(std::memory_order_acquire) == 0);
        tx.disconnect();
        rx.stop();
    }

    {
        std::atomic<int> messages{0};
        std::atomic<int> routes{0};
        Receiver rx;

        ReceiverOptions options;
        options.on_message = [&](const Message&) {
            messages.fetch_add(1, std::memory_order_release);
            rx.stop();
        };
        options.routes.push_back({"", [&](const Message&) {
            routes.fetch_add(1, std::memory_order_release);
        }});

        REQUIRE(rx.listen_with_options(0, std::move(options)));
        const auto port = rx.local_port();
        REQUIRE(port != 0);

        Sender tx;
        REQUIRE(tx.connect("127.0.0.1", port));

        Message msg("/fanout/message");
        msg.add(2);
        for (int i = 0; i < 100 && messages.load(std::memory_order_acquire) == 0; ++i) {
            REQUIRE(tx.send(msg));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        REQUIRE(messages.load(std::memory_order_acquire) > 0);
        REQUIRE(routes.load(std::memory_order_acquire) == 0);
        tx.disconnect();
        rx.stop();
    }

    {
        std::atomic<int> first_route{0};
        std::atomic<int> second_route{0};
        Receiver rx;

        ReceiverOptions options;
        options.routes.push_back({"", [&](const Message&) {
            first_route.fetch_add(1, std::memory_order_release);
            rx.stop();
        }});
        options.routes.push_back({"", [&](const Message&) {
            second_route.fetch_add(1, std::memory_order_release);
        }});

        REQUIRE(rx.listen_with_options(0, std::move(options)));
        const auto port = rx.local_port();
        REQUIRE(port != 0);

        Sender tx;
        REQUIRE(tx.connect("127.0.0.1", port));

        Message msg("/fanout/route");
        msg.add(3);
        for (int i = 0; i < 100
             && first_route.load(std::memory_order_acquire) == 0; ++i) {
            REQUIRE(tx.send(msg));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        REQUIRE(first_route.load(std::memory_order_acquire) > 0);
        REQUIRE(second_route.load(std::memory_order_acquire) == 0);
        tx.disconnect();
        rx.stop();
    }

    {
        std::atomic<int> first_message{0};
        std::atomic<int> second_message{0};
        Receiver rx;

        ReceiverOptions options;
        options.on_message = [&](const Message& msg) {
            if (msg.address == "/fanout/first") {
                first_message.fetch_add(1, std::memory_order_release);
                rx.stop();
            } else if (msg.address == "/fanout/second") {
                second_message.fetch_add(1, std::memory_order_release);
            }
        };

        REQUIRE(rx.listen_with_options(0, std::move(options)));
        const auto port = rx.local_port();
        REQUIRE(port != 0);

        Sender tx;
        REQUIRE(tx.connect("127.0.0.1", port));

        Bundle bundle;
        Message first("/fanout/first");
        first.add(4);
        bundle.add(std::move(first));
        Message second("/fanout/second");
        second.add(5);
        bundle.add(std::move(second));

        for (int i = 0; i < 100
             && first_message.load(std::memory_order_acquire) == 0; ++i) {
            REQUIRE(tx.send(bundle));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        REQUIRE(first_message.load(std::memory_order_acquire) > 0);
        REQUIRE(second_message.load(std::memory_order_acquire) == 0);
        tx.disconnect();
        rx.stop();
    }
}

TEST_CASE("OSC Receiver can be destroyed from a receiver callback",
          "[osc][udp][receiver][lifecycle]") {
    struct Owner {
        Receiver rx;
    };

    std::unique_ptr<Owner> owner = std::make_unique<Owner>();
    std::atomic<int> callbacks{0};
    std::atomic<bool> destroyed{false};

    ReceiverOptions options;
    options.on_message = [&](const Message& msg) {
        if (msg.address != "/callback/destroy") return;
        callbacks.fetch_add(1, std::memory_order_release);
        owner.reset();
        destroyed.store(true, std::memory_order_release);
    };

    REQUIRE(owner->rx.listen_with_options(0, std::move(options)));
    const auto port = owner->rx.local_port();
    REQUIRE(port != 0);

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", port));

    Message msg("/callback/destroy");
    msg.add(99);
    for (int i = 0; i < 100 && !destroyed.load(std::memory_order_acquire); ++i) {
        REQUIRE(tx.send(msg));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(callbacks.load(std::memory_order_acquire) > 0);
    REQUIRE(destroyed.load(std::memory_order_acquire));
    tx.disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

TEST_CASE("OSC Receiver with empty handler accepts datagrams until stopped",
          "[osc][udp][receiver]") {
    Receiver rx;
    REQUIRE(rx.listen(0, {}));
    REQUIRE(rx.is_listening());
    const auto port = rx.local_port();
    REQUIRE(port != 0);

    Sender tx;
    REQUIRE(tx.connect("127.0.0.1", port));
    Message msg("/receiver/no-handler");
    msg.add(123);
    REQUIRE(tx.send(msg));

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(rx.is_listening());
    REQUIRE(rx.local_port() == port);

    tx.disconnect();
    rx.stop();
    REQUIRE_FALSE(tx.is_connected());
    REQUIRE_FALSE(rx.is_listening());
}

TEST_CASE("OSC Receiver stop is idempotent", "[osc][udp][receiver]") {
    Receiver never_started;
    never_started.stop();
    REQUIRE_FALSE(never_started.is_listening());
    REQUIRE(never_started.local_port() == 0);

    Receiver rx;
    REQUIRE(rx.listen(0, [](const Message&) {}));
    REQUIRE(rx.is_listening());
    REQUIRE(rx.local_port() != 0);
    REQUIRE_FALSE(rx.listen(0, [](const Message&) {}));
    rx.stop();
    rx.stop();
    REQUIRE_FALSE(rx.is_listening());
    REQUIRE(rx.local_port() == 0);
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

TEST_CASE("OSC decode with malformed type tag marker keeps no-arg message",
          "[osc][codec][decode-edge]") {
    std::vector<uint8_t> buf;
    const char addr[] = "/no-comma";
    buf.insert(buf.end(), addr, addr + sizeof(addr));
    while (buf.size() % 4 != 0) buf.push_back(0);
    const char tags[] = "if";
    buf.insert(buf.end(), tags, tags + sizeof(tags));
    while (buf.size() % 4 != 0) buf.push_back(0);
    buf.insert(buf.end(), {0, 0, 0, 7, 0x3F, 0x80, 0, 0});

    auto decoded = decode(buf.data(), buf.size());
    REQUIRE(decoded.address == "/no-comma");
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

TEST_CASE("OSC decode of truncated int argument fails closed",
          "[osc][codec][decode-edge]") {
    Message msg("/t");
    msg.add(0x01020304);
    auto data = encode(msg);
    REQUIRE(data.size() >= 4);
    // Lop off the int bytes so the int tag points past the buffer end.
    auto truncated = std::vector<uint8_t>(data.begin(), data.end() - 4);
    auto decoded = decode(truncated.data(), truncated.size());
    REQUIRE(decoded.address.empty());   // malformed → empty-address sentinel
    REQUIRE(decoded.args.empty());
}

TEST_CASE("OSC decode of truncated float argument fails closed",
          "[osc][codec][decode-edge]") {
    Message msg("/f");
    msg.add(1.25f);
    auto data = encode(msg);
    REQUIRE(data.size() >= 4);
    // Keep the float tag but remove the payload bytes.
    auto truncated = std::vector<uint8_t>(data.begin(), data.end() - 4);
    auto decoded = decode(truncated.data(), truncated.size());
    REQUIRE(decoded.address.empty());
    REQUIRE(decoded.args.empty());
}

TEST_CASE("OSC decode of truncated blob payload fails closed",
          "[osc][codec][decode-edge]") {
    Message msg("/b");
    msg.add(std::vector<uint8_t>{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF});
    auto data = encode(msg);
    // Chop off the trailing blob bytes so the prefixed size walks off the end.
    auto truncated = std::vector<uint8_t>(data.begin(), data.begin() + (data.size() - 4));
    auto decoded = decode(truncated.data(), truncated.size());
    REQUIRE(decoded.address.empty());
    REQUIRE(decoded.args.empty());
}

TEST_CASE("OSC decode of negative blob size fails closed",
          "[osc][codec][decode-edge]") {
    std::vector<uint8_t> buf;
    const char addr[] = "/bad-blob";
    buf.insert(buf.end(), addr, addr + sizeof(addr));
    while (buf.size() % 4 != 0) buf.push_back(0);
    const char tags[] = ",b";
    buf.insert(buf.end(), tags, tags + sizeof(tags));
    while (buf.size() % 4 != 0) buf.push_back(0);
    const uint8_t negative_size[] = {0xFF, 0xFF, 0xFF, 0xFF};
    buf.insert(buf.end(), negative_size, negative_size + sizeof(negative_size));

    auto decoded = decode(buf.data(), buf.size());
    REQUIRE(decoded.address.empty());   // negative/impossible blob size → malformed
    REQUIRE(decoded.args.empty());
}

TEST_CASE("OSC decode of unterminated string argument fails closed",
          "[osc][codec][decode-edge]") {
    std::vector<uint8_t> buf;
    const char addr[] = "/string";
    buf.insert(buf.end(), addr, addr + sizeof(addr));
    while (buf.size() % 4 != 0) buf.push_back(0);
    const char tags[] = ",s";
    buf.insert(buf.end(), tags, tags + sizeof(tags));
    while (buf.size() % 4 != 0) buf.push_back(0);
    buf.insert(buf.end(), {'u', 'n', 't', 'e', 'r', 'm'});  // no null terminator

    auto decoded = decode(buf.data(), buf.size());
    REQUIRE(decoded.address.empty());   // unterminated string payload → malformed
    REQUIRE(decoded.args.empty());
}

TEST_CASE("OSC decode of truncated color argument fails closed",
          "[osc][codec][decode-edge]") {
    std::vector<uint8_t> buf;
    const char addr[] = "/c";
    buf.insert(buf.end(), addr, addr + sizeof(addr));
    while (buf.size() % 4 != 0) buf.push_back(0);
    const char tags[] = ",r";
    buf.insert(buf.end(), tags, tags + sizeof(tags));
    while (buf.size() % 4 != 0) buf.push_back(0);
    buf.insert(buf.end(), {0x11, 0x22});  // only 2 of the 4 color bytes

    auto decoded = decode(buf.data(), buf.size());
    REQUIRE(decoded.address.empty());   // truncated 32-bit color → malformed
    REQUIRE(decoded.args.empty());
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

TEST_CASE("OSC encode preserves moved string and blob arguments",
          "[osc][codec]") {
    std::string label = "moved-label";
    std::vector<uint8_t> payload{0x10, 0x20, 0x30, 0x40};

    Message msg("/move");
    msg.add(std::move(label)).add(std::move(payload));

    auto data = encode(msg);
    auto decoded = decode(data.data(), data.size());
    REQUIRE(decoded.address == "/move");
    REQUIRE(decoded.get_string(0) == "moved-label");
    REQUIRE(std::get<std::vector<uint8_t>>(decoded.args[1])
            == std::vector<uint8_t>{0x10, 0x20, 0x30, 0x40});
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

TEST_CASE("OSC Sender::send bundle without connect is rejected",
          "[osc][udp][sender][bundle]") {
    Sender tx;
    REQUIRE_FALSE(tx.is_connected());

    Bundle bundle;
    Message msg("/bundle/unconnected");
    msg.add(1);
    bundle.add(std::move(msg));

    REQUIRE_FALSE(tx.send(bundle));
}

TEST_CASE("OSC decode preserves empty strings and padded string arguments", "[osc][codec][issue-644]") {
    Message msg("/strings");
    msg.add(std::string{});
    msg.add(std::string{"abc"});
    msg.add(std::string{"abcd"});

    auto data = encode(msg);
    REQUIRE(data.size() % 4 == 0);

    auto decoded = decode(data.data(), data.size());
    REQUIRE(decoded.address == "/strings");
    REQUIRE(decoded.args.size() == 3);
    REQUIRE(decoded.get_string(0, "fallback").empty());
    REQUIRE(decoded.get_string(1) == "abc");
    REQUIRE(decoded.get_string(2) == "abcd");
}

TEST_CASE("OSC decode of string argument with no payload fails closed", "[osc][codec][issue-644]") {
    std::vector<uint8_t> buf;
    const char addr[] = "/truncated";
    buf.insert(buf.end(), addr, addr + sizeof(addr));
    while (buf.size() % 4 != 0) buf.push_back(0);
    const char tags[] = ",s";
    buf.insert(buf.end(), tags, tags + sizeof(tags));
    while (buf.size() % 4 != 0) buf.push_back(0);
    // ',s' tag but no string payload at all → malformed, not an empty-string arg.

    auto decoded = decode(buf.data(), buf.size());
    REQUIRE(decoded.address.empty());
    REQUIRE(decoded.args.empty());
}

TEST_CASE("OSC decode tolerates extra trailing padding bytes", "[osc][codec][issue-644]") {
    Message msg("/trail");
    msg.add(123);
    auto data = encode(msg);
    data.insert(data.end(), {0, 0, 0, 0});

    auto decoded = decode(data.data(), data.size());
    REQUIRE(decoded.address == "/trail");
    REQUIRE(decoded.args.size() == 1);
    REQUIRE(decoded.get_int(0) == 123);
}

TEST_CASE("OSC decode keeps explicit defaults for out-of-range access",
          "[osc][message]") {
    Message msg("/defaults");
    msg.add(12).add(std::string("name"));

    REQUIRE(msg.get_int(4, -9) == -9);
    REQUIRE_THAT(msg.get_float(4, -0.5f), WithinAbs(-0.5f, 1e-6f));
    REQUIRE(msg.get_string(4, "fallback") == "fallback");
    REQUIRE(msg.get_string(0, "wrong") == "wrong");
}

TEST_CASE("OSC Sender::send_raw without connect is rejected", "[osc][udp][sender][issue-644]") {
    Sender tx;
    uint8_t payload[] = {0, 1, 2, 3};
    REQUIRE_FALSE(tx.is_connected());
    REQUIRE_FALSE(tx.send_raw(payload, sizeof(payload)));
    tx.disconnect();
    REQUIRE_FALSE(tx.is_connected());
}

TEST_CASE("OSC Receiver accepts an empty handler and stops cleanly",
          "[osc][udp][receiver]") {
    constexpr uint16_t port = 29879;

    Receiver rx;
    REQUIRE(rx.listen(port, {}));
    REQUIRE(rx.is_listening());
    rx.stop();
    REQUIRE_FALSE(rx.is_listening());
}

TEST_CASE("OSC Receiver rejects binding to an already-used UDP port",
          "[osc][udp][receiver]") {
    Receiver first;
    REQUIRE(first.listen(0, [](const Message&) {}));
    const auto port = first.local_port();
    REQUIRE(port != 0);

    Receiver second;
    REQUIRE_FALSE(second.listen(port, [](const Message&) {}));
    REQUIRE_FALSE(second.is_listening());
    REQUIRE(second.local_port() == 0);
    REQUIRE(first.is_listening());

    first.stop();
    REQUIRE_FALSE(first.is_listening());
}

// ── Bundles and address patterns ────────────────────────────────────────────

TEST_CASE("OSC bundle serializes and restores nested messages and bundles",
          "[osc][bundle]") {
    Bundle nested;
    Message nested_msg("/nested");
    nested_msg.add(std::string("ok"));
    nested.add(std::move(nested_msg));

    Bundle bundle;
    bundle.timetag = TimeTag::from_unix(1'700'000'000.25);
    Message root_msg("/root");
    root_msg.add(17);
    bundle.add(std::move(root_msg));
    bundle.add(std::move(nested));

    auto data = bundle.serialize();
    auto decoded = Bundle::deserialize(data.data(), data.size());

    REQUIRE(decoded.has_value());
    REQUIRE(decoded->timetag == bundle.timetag);
    REQUIRE(decoded->elements.size() == 2);
    REQUIRE(decoded->elements[0].is_message());
    REQUIRE(decoded->elements[0].message().address == "/root");
    REQUIRE(decoded->elements[0].message().get_int(0) == 17);
    REQUIRE(decoded->elements[1].is_bundle());
    REQUIRE(decoded->elements[1].bundle().elements.size() == 1);
    REQUIRE(decoded->elements[1].bundle().elements[0].message().address == "/nested");
    REQUIRE(decoded->elements[1].bundle().elements[0].message().get_string(0) == "ok");
}

TEST_CASE("OSC bundle rejects malformed element boundaries",
          "[osc][bundle]") {
    Bundle bundle;
    Message msg("/one");
    msg.add(1);
    bundle.add(std::move(msg));

    auto data = bundle.serialize();
    REQUIRE(Bundle::deserialize(nullptr, 0) == std::nullopt);

    auto truncated = data;
    truncated.pop_back();
    REQUIRE(Bundle::deserialize(truncated.data(), truncated.size()) == std::nullopt);

    auto oversized = data;
    oversized[16] = 0x7F;
    REQUIRE(Bundle::deserialize(oversized.data(), oversized.size()) == std::nullopt);

    std::vector<uint8_t> bad_header(16, 0);
    REQUIRE(Bundle::deserialize(bad_header.data(), bad_header.size()) == std::nullopt);
}

TEST_CASE("OSC address pattern rejects incomplete classes and alternatives",
          "[osc][pattern]") {
    REQUIRE(address_matches("/note/[0-9]", "/note/7"));
    REQUIRE(address_matches("/note/[!0-3]", "/note/8"));
    REQUIRE_FALSE(address_matches("/note/[!0-3]", "/note/2"));
    REQUIRE_FALSE(address_matches("/note/[0-9", "/note/7"));

    REQUIRE(address_matches("/voice/{lead,bass}", "/voice/bass"));
    REQUIRE_FALSE(address_matches("/voice/{lead,bass", "/voice/bass"));
    REQUIRE_FALSE(address_matches("/voice/{lead,bass}", "/voice/pad"));

    REQUIRE(address_matches("/track/?", "/track/A"));
    REQUIRE_FALSE(address_matches("/track/?", "/track//"));
}

TEST_CASE("OSC address pattern covers boundary backtracking failures",
          "[osc][pattern]") {
    REQUIRE(address_matches("/mix/*/tail", "/mix//tail"));
    REQUIRE_FALSE(address_matches("/mix/*/tail", "/mix/a/b/tail"));
    REQUIRE_FALSE(address_matches("/mix/*tail", "/mix/a/tail"));
    REQUIRE_FALSE(address_matches("/mix/*tail", "/mix/tail/extra"));

    REQUIRE_FALSE(address_matches("/note/[abc]", "/note/"));
    REQUIRE_FALSE(address_matches("/note/[!abc]", "/note/a"));
    REQUIRE(address_matches("/note/[!abc]", "/note/z"));
    REQUIRE_FALSE(address_matches("/note/[z-a]", "/note/m"));

    REQUIRE(address_matches("/{alpha,beta,gamma}/1", "/alpha/1"));
    REQUIRE(address_matches("/{alpha,beta,gamma}/1", "/gamma/1"));
    REQUIRE_FALSE(address_matches("/{alpha,beta,gamma}/1", "/gamma/2"));
    REQUIRE_FALSE(address_matches("/{alpha,,gamma}/1", "/gamma/1"));
}

// ── OSC 'r' type tag — RGBA color argument ─────────────────────────
// Closes the gap-doc OSC row "Message + Bundle + Argument (..., color)".

TEST_CASE("OSC encode/decode round-trips RGBA color 'r' tag",
          "[osc][color]") {
    Message msg("/light/color");
    msg.add(ColorRgba{0x11, 0x22, 0x33, 0x44});

    auto bytes = encode(msg);
    auto decoded = decode(bytes.data(), bytes.size());
    REQUIRE(decoded.address == "/light/color");
    REQUIRE(decoded.args.size() == 1);
    auto* c = std::get_if<ColorRgba>(&decoded.args[0]);
    REQUIRE(c != nullptr);
    REQUIRE(c->r == 0x11);
    REQUIRE(c->g == 0x22);
    REQUIRE(c->b == 0x33);
    REQUIRE(c->a == 0x44);

    REQUIRE(decoded.get_colour(0) == ColorRgba{0x11, 0x22, 0x33, 0x44});
    // Default fallback when wrong index or wrong type.
    REQUIRE(decoded.get_colour(5).a == 0xff);
}

TEST_CASE("OSC mixes RGBA color with other arg types",
          "[osc][color]") {
    Message msg("/scene/set");
    msg.add(int32_t(7));
    msg.add(ColorRgba{255, 0, 0, 255});
    msg.add(std::string("red"));

    auto bytes = encode(msg);
    auto decoded = decode(bytes.data(), bytes.size());
    REQUIRE(decoded.args.size() == 3);
    REQUIRE(decoded.get_int(0) == 7);
    REQUIRE(decoded.get_colour(1) == ColorRgba{255, 0, 0, 255});
    REQUIRE(decoded.get_string(2) == "red");
}
