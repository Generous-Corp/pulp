// Headless tests for UmpSession / UmpEndpoint / VirtualUmpEndpoint
// (macOS plan item 8.1).
//
// These tests deliberately stay in-process: we never open a real
// CoreMIDI source/destination so the suite runs on Linux/Windows/CI
// without a MIDI Studio. The OS-backed half (CoreMIDI 2.0 client
// init + endpoint enumeration) is exercised by the platform .mm
// linked into `pulp-midi` — when this test target runs on macOS the
// session's `os_backend_active()` is expected to be true; on other
// platforms it should be false and virtual endpoints carry the suite.

#include <pulp/midi/ump.hpp>
#include <pulp/midi/ump_endpoint.hpp>
#include <pulp/midi/ump_session.hpp>
#include <pulp/midi/ump_virtual_endpoint.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace pulp::midi;

namespace {

UmpPacket make_midi2_note_on(uint8_t group, uint8_t channel,
                             uint8_t note, uint16_t velocity_16) {
    UmpPacket p;
    p.word_count = 2;
    // Type 0x4 (MIDI 2.0 channel voice), status 0x9 (Note On).
    p.words[0] = (uint32_t(0x4) << 28)
               | (uint32_t(group & 0x0F) << 24)
               | (uint32_t(0x90) << 16)
               | (uint32_t(channel & 0x0F) << 16)
               | (uint32_t(note & 0x7F) << 8);
    p.words[1] = uint32_t(velocity_16) << 16;
    return p;
}

} // namespace

// ── VirtualUmpEndpoint ──────────────────────────────────────────────

TEST_CASE("VirtualUmpEndpoint advertises supplied config",
          "[midi][ump][session][item-8.1]") {
    VirtualUmpEndpointConfig cfg;
    cfg.name = "test-endpoint";
    cfg.loopback = false;
    cfg.direction = { true, false };

    VirtualUmpEndpoint ep(std::move(cfg));
    REQUIRE(ep.info().id == "test-endpoint");
    REQUIRE(ep.info().name == "test-endpoint");
    REQUIRE(ep.info().is_virtual);
    REQUIRE(ep.info().direction.can_receive);
    REQUIRE_FALSE(ep.info().direction.can_send);
    REQUIRE(ep.is_open());
}

TEST_CASE("VirtualUmpEndpoint rejects send when can_send=false",
          "[midi][ump][session][item-8.1]") {
    VirtualUmpEndpointConfig cfg;
    cfg.name = "sink-only";
    cfg.direction = { true, false };
    VirtualUmpEndpoint ep(std::move(cfg));

    REQUIRE_FALSE(ep.send(make_midi2_note_on(0, 0, 60, 0x8000)));
    REQUIRE(ep.sent_count() == 0);
}

TEST_CASE("VirtualUmpEndpoint counts sends + delivers",
          "[midi][ump][session][item-8.1]") {
    VirtualUmpEndpoint ep({"counts", false, {true, true}});

    REQUIRE(ep.send(make_midi2_note_on(0, 0, 60, 0x8000)));
    REQUIRE(ep.sent_count() == 1);
    REQUIRE(ep.delivered_count() == 0);   // no callback installed

    UmpPacket got{};
    int callbacks = 0;
    ep.set_receive_callback([&](const UmpPacket& p, double /*ts*/) {
        got = p;
        callbacks++;
    });
    REQUIRE(ep.deliver(make_midi2_note_on(1, 2, 72, 0x4000)));
    REQUIRE(callbacks == 1);
    REQUIRE(got.word_count == 2);
    REQUIRE(((got.words[0] >> 28) & 0x0F) == 0x4);
    REQUIRE(ep.delivered_count() == 1);
}

TEST_CASE("VirtualUmpEndpoint loopback fires callback on send",
          "[midi][ump][session][item-8.1]") {
    VirtualUmpEndpoint ep({"loop", /*loopback=*/true, {true, true}});

    int callbacks = 0;
    ep.set_receive_callback([&](const UmpPacket&, double) { callbacks++; });

    REQUIRE(ep.send(make_midi2_note_on(0, 0, 60, 0x8000)));
    REQUIRE(callbacks == 1);
    REQUIRE(ep.sent_count() == 1);
    REQUIRE(ep.delivered_count() == 1);
}

TEST_CASE("VirtualUmpEndpoint close blocks further send/deliver",
          "[midi][ump][session][item-8.1]") {
    VirtualUmpEndpoint ep({"closing", true, {true, true}});
    int callbacks = 0;
    ep.set_receive_callback([&](const UmpPacket&, double) { callbacks++; });

    ep.close();
    REQUIRE_FALSE(ep.is_open());
    REQUIRE_FALSE(ep.send(make_midi2_note_on(0, 0, 60, 0x8000)));
    REQUIRE_FALSE(ep.deliver(make_midi2_note_on(0, 0, 60, 0x8000)));
    REQUIRE(callbacks == 0);
}

// ── UmpSession (virtual-only path) ──────────────────────────────────

TEST_CASE("UmpSession creates + tears down without OS backend",
          "[midi][ump][session][item-8.1]") {
    UmpSessionConfig cfg;
    cfg.name = "no-os";
    cfg.enable_os_backend = false;
    UmpSession session(std::move(cfg));
    REQUIRE_FALSE(session.os_backend_active());
    REQUIRE(session.virtual_endpoint_count() == 0);
    REQUIRE(session.enumerate_endpoints().empty());
}

TEST_CASE("UmpSession registers + enumerates virtual endpoints",
          "[midi][ump][session][item-8.1]") {
    UmpSession session({"vsession", /*enable_os_backend=*/false});

    auto a = session.register_virtual_endpoint({"alpha", false, {true, true}});
    auto b = session.register_virtual_endpoint({"beta", true, {true, true}});
    REQUIRE(session.virtual_endpoint_count() == 2);

    auto endpoints = session.enumerate_endpoints();
    REQUIRE(endpoints.size() == 2);
    bool saw_alpha = false, saw_beta = false;
    for (const auto& ep : endpoints) {
        if (ep.id == "alpha") { saw_alpha = true; REQUIRE(ep.is_virtual); }
        if (ep.id == "beta")  { saw_beta  = true; REQUIRE(ep.is_virtual); }
    }
    REQUIRE(saw_alpha);
    REQUIRE(saw_beta);

    REQUIRE(session.unregister_virtual_endpoint("alpha"));
    REQUIRE_FALSE(session.unregister_virtual_endpoint("alpha")); // idempotent
    REQUIRE(session.virtual_endpoint_count() == 1);
}

TEST_CASE("UmpSession open_endpoint returns virtual endpoints by id",
          "[midi][ump][session][item-8.1]") {
    UmpSession session({"open-virt", false});
    auto ep = session.register_virtual_endpoint({"target", false, {true, true}});

    UmpOpenStatus status = UmpOpenStatus::OsError;
    UmpEndpoint* h = session.open_endpoint("target", &status);
    REQUIRE(h != nullptr);
    REQUIRE(h == ep.get());
    REQUIRE(status == UmpOpenStatus::Ok);

    status = UmpOpenStatus::Ok;
    h = session.open_endpoint("missing", &status);
    REQUIRE(h == nullptr);
    // No OS backend → OsBackendUnavailable wins over NotFound; either is
    // a legitimate "couldn't open" outcome but we want callers to be able
    // to distinguish "backend offline" from "id was wrong".
    REQUIRE(status == UmpOpenStatus::OsBackendUnavailable);
}

TEST_CASE("UmpSession wire_virtual_loopback round-trips a packet",
          "[midi][ump][session][item-8.1]") {
    UmpSession session({"wired", false});
    auto src = session.register_virtual_endpoint({"src", false, {true, true}});
    auto dst = session.register_virtual_endpoint({"dst", false, {true, true}});

    int seen = 0;
    UmpPacket received{};
    dst->set_receive_callback([&](const UmpPacket& p, double) {
        received = p;
        seen++;
    });

    REQUIRE(session.wire_virtual_loopback("src", "dst"));
    // Feeding `src` simulates an upstream "device" delivering a packet
    // into the session; the wire forwards it to `dst`'s receiver.
    REQUIRE(src->deliver(make_midi2_note_on(0, 0, 64, 0xC000)));
    REQUIRE(seen == 1);
    REQUIRE(received.word_count == 2);
    // Same channel-voice MIDI 2.0 status nibble.
    REQUIRE(((received.words[0] >> 16) & 0xF0) == 0x90);
}

TEST_CASE("UmpSession wire_virtual_loopback rejects unknown endpoints",
          "[midi][ump][session][item-8.1]") {
    UmpSession session({"wired-missing", false});
    session.register_virtual_endpoint({"only-one", false, {true, true}});
    REQUIRE_FALSE(session.wire_virtual_loopback("missing", "only-one"));
    REQUIRE_FALSE(session.wire_virtual_loopback("only-one", "missing"));
}

TEST_CASE("UmpSession survives concurrent virtual register + enumerate",
          "[midi][ump][session][item-8.1][thread]") {
    // Light TSan-style smoke: a writer thread spams register/unregister
    // while a reader thread spams enumerate. We don't assert specific
    // counts (race-tolerant), only that no crash + final state is
    // reachable. Catches obvious mutex-omission bugs.
    UmpSession session({"concurrent", false});
    std::atomic<bool> stop{false};

    std::thread writer([&] {
        for (int i = 0; i < 200 && !stop.load(); ++i) {
            VirtualUmpEndpointConfig cfg;
            cfg.name = "ep-" + std::to_string(i);
            session.register_virtual_endpoint(std::move(cfg));
            if ((i & 1) == 0) {
                session.unregister_virtual_endpoint("ep-" + std::to_string(i / 2));
            }
        }
    });
    std::thread reader([&] {
        for (int i = 0; i < 200 && !stop.load(); ++i) {
            auto snapshot = session.enumerate_endpoints();
            (void)snapshot;
        }
    });
    writer.join();
    reader.join();
    stop.store(true);

    // Final sanity: count is internally consistent with enumerate size
    // (we only asserted there's no crash; this guards the invariant).
    auto endpoints = session.enumerate_endpoints();
    REQUIRE(endpoints.size() == session.virtual_endpoint_count());
}

// ── UmpSession (OS backend path, opportunistic on macOS) ────────────

TEST_CASE("UmpSession os_backend_active reflects requested config",
          "[midi][ump][session][item-8.1]") {
    UmpSession off({"os-off", /*enable_os_backend=*/false});
    REQUIRE_FALSE(off.os_backend_active());

#if defined(__APPLE__) && !defined(TARGET_OS_IOS)
    // On macOS with a real CoreMIDI server we expect the backend to
    // come up; on CI sandboxes that strip MIDI entitlements
    // MIDIClientCreate may still fail and we just verify the session
    // didn't crash and falls back to virtual-only.
    UmpSession on({"os-on", /*enable_os_backend=*/true});
    // Either active (normal macOS) or inactive (sandbox); both are OK.
    auto endpoints = on.enumerate_endpoints();
    (void)endpoints;
#endif
}
