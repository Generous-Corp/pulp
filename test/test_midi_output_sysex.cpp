// MidiOutput::send_sysex: framing validation, the mock backend's recording,
// large messages, the default for a backend with no SysEx output, and (on
// macOS) delivery through the real CoreMIDI framework to an in-process virtual
// destination, so no hardware is needed.

#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/device.hpp>
#include <pulp/midi/mock_midi_system.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <CoreMIDI/CoreMIDI.h>
#endif

using namespace pulp::midi;

namespace {

std::vector<std::uint8_t> message(std::size_t payload, std::uint8_t fill = 0x15) {
    std::vector<std::uint8_t> m;
    m.push_back(0xF0);
    for (std::size_t i = 0; i < payload; ++i)
        m.push_back(static_cast<std::uint8_t>((fill + i) & 0x7F));
    m.push_back(0xF7);
    return m;
}

// An output that does not override send_sysex.
struct PlainOutput : MidiOutput {
    bool open(const std::string&) override {
        return open_ = true;
    }
    void close() override {
        open_ = false;
    }
    bool is_open() const override {
        return open_;
    }
    void send(const MidiEvent&) override {}
    bool open_ = false;
};

} // namespace

TEST_CASE("sysex framing: only a complete F0 ... F7 message of data bytes is valid",
          "[midi][sysex][output]") {
    CHECK(is_valid_sysex_message(message(0))); // F0 F7
    CHECK(is_valid_sysex_message(message(1)));
    CHECK(is_valid_sysex_message(message(300)));

    const std::vector<std::uint8_t> empty;
    CHECK_FALSE(is_valid_sysex_message(empty));
    CHECK_FALSE(is_valid_sysex_message(std::vector<std::uint8_t>{0xF0}));
    CHECK_FALSE(is_valid_sysex_message(std::vector<std::uint8_t>{0xF7}));
    CHECK_FALSE(is_valid_sysex_message(std::vector<std::uint8_t>{0x01, 0x02, 0xF7})); // no F0
    CHECK_FALSE(is_valid_sysex_message(std::vector<std::uint8_t>{0xF0, 0x01, 0x02})); // no F7
    CHECK_FALSE(is_valid_sysex_message(std::vector<std::uint8_t>{0xF7, 0x01, 0xF0})); // reversed
    CHECK_FALSE(is_valid_sysex_message(std::vector<std::uint8_t>{0xF0, 0x01, 0x90, 0x02, 0xF7}));
    CHECK_FALSE(is_valid_sysex_message(std::vector<std::uint8_t>{0xF0, 0x01, 0xF0, 0x02, 0xF7}));
    CHECK_FALSE(is_valid_sysex_message(std::vector<std::uint8_t>{0xF0, 0x80, 0xF7}));

    // The size bound is inclusive.
    CHECK(is_valid_sysex_message(message(kMaxSysexSendBytes - 2)));
    CHECK_FALSE(is_valid_sysex_message(message(kMaxSysexSendBytes - 1)));
}

TEST_CASE("mock output records each sysex message in order", "[midi][sysex][output]") {
    MockMidiSystem system;
    system.add_port("out", "Out", false);
    auto out = system.create_output();
    REQUIRE(out->open("out"));

    const auto a = message(3, 0x10);
    const auto b = message(5, 0x20);
    CHECK(out->send_sysex(a));
    CHECK(out->send_sysex(b));
    const auto sent = system.sent_sysex();
    REQUIRE(sent.size() == 2);
    CHECK(sent[0] == a);
    CHECK(sent[1] == b);

    system.clear_sent_sysex();
    CHECK(system.sent_sysex().empty());
}

TEST_CASE("mock output rejects malformed and unopened sends and records nothing",
          "[midi][sysex][output]") {
    MockMidiSystem system;
    auto out = system.create_output();

    CHECK_FALSE(out->send_sysex(message(4))); // not open yet
    REQUIRE(out->open("out"));
    CHECK_FALSE(out->send_sysex(std::vector<std::uint8_t>{0xF0, 0x01, 0x02}));
    CHECK_FALSE(out->send_sysex(std::vector<std::uint8_t>{0xF0, 0x90, 0xF7}));
    CHECK_FALSE(out->send_sysex(std::vector<std::uint8_t>{}));
    out->close();
    CHECK_FALSE(out->send_sysex(message(4))); // closed again
    CHECK(system.sent_sysex().empty());
}

TEST_CASE("mock output carries large messages intact", "[midi][sysex][output]") {
    MockMidiSystem system;
    auto out = system.create_output();
    REQUIRE(out->open("out"));
    for (std::size_t payload : {255u, 256u, 257u, 1000u, 70000u}) {
        const auto big = message(payload);
        INFO("payload " << payload);
        CHECK(out->send_sysex(big));
        const auto sent = system.sent_sysex();
        REQUIRE_FALSE(sent.empty());
        CHECK(sent.back() == big);
    }
}

TEST_CASE("the caller's buffer is copied before send_sysex returns", "[midi][sysex][output]") {
    MockMidiSystem system;
    auto out = system.create_output();
    REQUIRE(out->open("out"));
    auto buf = message(8, 0x30);
    const auto original = buf;
    REQUIRE(out->send_sysex(buf));
    std::fill(buf.begin() + 1, buf.end() - 1, std::uint8_t{0});
    CHECK(system.sent_sysex().back() == original);
}

TEST_CASE("a backend without sysex output reports unsupported", "[midi][sysex][output]") {
    PlainOutput out;
    REQUIRE(out.open(""));
    CHECK_FALSE(out.send_sysex(message(4)));
}

#if defined(__APPLE__)

namespace {

constexpr SInt32 kDestId = 0x50535859; // arbitrary, unlikely to collide

// An in-process virtual destination that collects the sysex bytes it receives.
struct VirtualDestination {
    MIDIClientRef client = 0;
    MIDIEndpointRef dest = 0;
    std::mutex mu;
    std::vector<std::uint8_t> received;

    bool create() {
        if (MIDIClientCreate(CFSTR("PulpSysexTest"), nullptr, nullptr, &client) != noErr)
            return false;
        const OSStatus st = MIDIDestinationCreateWithBlock(
            client, CFSTR("Pulp Sysex Destination"), &dest, ^(const MIDIPacketList* list, void*) {
              std::lock_guard<std::mutex> lk(mu);
              const MIDIPacket* p = &list->packet[0];
              for (UInt32 i = 0; i < list->numPackets; ++i) {
                  received.insert(received.end(), p->data, p->data + p->length);
                  p = MIDIPacketNext(p);
              }
            });
        if (st != noErr)
            return false;
        MIDIObjectSetIntegerProperty(dest, kMIDIPropertyUniqueID, kDestId);
        return true;
    }

    std::size_t size() {
        std::lock_guard<std::mutex> lk(mu);
        return received.size();
    }

    ~VirtualDestination() {
        if (dest)
            MIDIEndpointDispose(dest);
        if (client)
            MIDIClientDispose(client);
    }
};

// CoreMIDI paces MIDISendSysex at MIDI wire speed (about 3.1 KB/s, measured:
// 3000 bytes took 0.93 s to an in-process destination), so the wait scales with
// the bytes expected instead of using a fixed timeout.
bool wait_for_bytes(VirtualDestination& d, std::size_t n) {
    const auto limit = std::chrono::milliseconds(3000 + static_cast<long>(n) / 2);
    const auto start = std::chrono::steady_clock::now();
    while (d.size() < n && std::chrono::steady_clock::now() - start < limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return d.size() >= n;
}

} // namespace

TEST_CASE("CoreMIDI output delivers sysex, including over 256 bytes, to a destination",
          "[midi][sysex][output][coremidi]") {
    VirtualDestination dest;
    if (!dest.create())
        SKIP("no CoreMIDI server reachable");
    auto system = create_midi_system();
    auto out = system->create_output();
    if (!out->open(std::to_string(kDestId)))
        SKIP("the virtual destination was not visible to the output");

    const auto small = message(10, 0x11);
    REQUIRE(out->send_sysex(small));
    REQUIRE(wait_for_bytes(dest, small.size()));
    {
        std::lock_guard<std::mutex> lk(dest.mu);
        CHECK(dest.received == small);
        dest.received.clear();
    }

    const auto big = message(600, 0x22);
    REQUIRE(out->send_sysex(big));
    REQUIRE(wait_for_bytes(dest, big.size()));
    std::lock_guard<std::mutex> lk(dest.mu);
    CHECK(dest.received == big);
}

TEST_CASE("CoreMIDI output refuses malformed and closed sends", "[midi][sysex][output][coremidi]") {
    VirtualDestination dest;
    if (!dest.create())
        SKIP("no CoreMIDI server reachable");
    auto system = create_midi_system();
    auto out = system->create_output();
    CHECK_FALSE(out->send_sysex(message(4))); // not open
    if (!out->open(std::to_string(kDestId)))
        SKIP("the virtual destination was not visible to the output");
    CHECK_FALSE(out->send_sysex(std::vector<std::uint8_t>{0xF0, 0x01, 0x02}));
    CHECK_FALSE(out->send_sysex(std::vector<std::uint8_t>{0xF0, 0x90, 0xF7}));
    out->close();
    CHECK_FALSE(out->send_sysex(message(4)));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(dest.size() == 0);
}

TEST_CASE("CoreMIDI output frees every request and bounds the queue",
          "[midi][sysex][output][coremidi]") {
    VirtualDestination dest;
    if (!dest.create())
        SKIP("no CoreMIDI server reachable");
    auto system = create_midi_system();
    auto out = system->create_output();
    if (!out->open(std::to_string(kDestId)))
        SKIP("the virtual destination was not visible to the output");

    // A burst larger than the in-flight bound: some are accepted and, once
    // the earlier ones complete, the next send is accepted again.
    const auto m = message(400, 0x05);
    int accepted = 0;
    for (int i = 0; i < 64; ++i)
        accepted += out->send_sysex(m) ? 1 : 0;
    CHECK(accepted >= 1);
    CHECK(accepted < 64); // the bound refused some of the burst
    REQUIRE(wait_for_bytes(dest, static_cast<std::size_t>(accepted) * m.size()));
    // Completed requests no longer count against the bound.
    bool again = false;
    for (int i = 0; i < 200 && !again; ++i) {
        again = out->send_sysex(m);
        if (!again)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(again);
}

#endif

// ── The in-flight bound (runs everywhere; the CoreMIDI backend uses this gate) ──

#include <pulp/midi/sysex_send_gate.hpp>

TEST_CASE("sysex gate: a send past the bound is refused, nothing is lost, and slots come back",
          "[midi][sysex][output][gate]") {
    SysexSendGate gate(16);
    CHECK(gate.max_in_flight() == 16);

    // The first 16 are accepted.
    for (int i = 0; i < 16; ++i)
        REQUIRE(gate.try_acquire());
    CHECK(gate.in_flight() == 16);

    // The 17th and every one after it is refused, and a refusal takes no slot,
    // so a later release is not shadowed by a phantom count.
    for (int i = 0; i < 100; ++i)
        CHECK_FALSE(gate.try_acquire());
    CHECK(gate.in_flight() == 16);

    // Each completion frees exactly one slot.
    gate.release();
    CHECK(gate.in_flight() == 15);
    CHECK(gate.try_acquire());
    CHECK_FALSE(gate.try_acquire());

    // Everything completes: the count returns to zero, with no leaked slot.
    for (int i = 0; i < 16; ++i)
        gate.release();
    CHECK(gate.in_flight() == 0);
    for (int i = 0; i < 16; ++i)
        CHECK(gate.try_acquire());
}

TEST_CASE("sysex gate: concurrent senders never exceed the bound", "[midi][sysex][output][gate]") {
    SysexSendGate gate(16);
    std::atomic<int> accepted{0};
    std::atomic<int> over{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < 20000; ++i) {
                if (gate.try_acquire()) {
                    if (gate.in_flight() > 16)
                        ++over;
                    ++accepted;
                    gate.release();
                }
            }
        });
    }
    for (auto& w : workers)
        w.join();
    CHECK(over.load() == 0);
    CHECK(gate.in_flight() == 0);
    CHECK(accepted.load() > 0);
}
