#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/midi_ci.hpp>

using namespace pulp::midi;

TEST_CASE("MUID generate is non-zero", "[midi][ci]") {
    auto muid = MUID::generate();
    REQUIRE(muid.value > 0);
    REQUIRE(muid.value < 0x0FFFFFFF);
}

TEST_CASE("MUID broadcast", "[midi][ci]") {
    auto bc = MUID::broadcast();
    REQUIRE(bc.is_broadcast());
    REQUIRE(bc.value == 0x0FFFFFFF);
}

TEST_CASE("CiDiscovery creates valid inquiry", "[midi][ci]") {
    CiDiscovery ci;
    ci.set_device_info({ci.local_muid(), 0x123, 0x01, 0x02, 0x0100, 2, 128});

    auto msg = ci.create_discovery_inquiry();
    REQUIRE(msg.size() > 14);
    REQUIRE(msg.front() == 0xF0);  // SysEx start
    REQUIRE(msg.back() == 0xF7);   // SysEx end
    REQUIRE(msg[1] == 0x7E);       // Universal SysEx
    REQUIRE(msg[3] == 0x0D);       // CI sub-ID
    REQUIRE(msg[4] == 0x70);       // Discovery inquiry
}

TEST_CASE("CiDiscovery responds to inquiry", "[midi][ci]") {
    CiDiscovery responder;
    CiDiscovery inquirer;

    auto inquiry = inquirer.create_discovery_inquiry();
    auto reply = responder.process_message(inquiry.data(), inquiry.size());

    REQUIRE_FALSE(reply.empty());
    REQUIRE(reply.front() == 0xF0);
    REQUIRE(reply[4] == 0x71);  // Discovery reply
}

TEST_CASE("CiDiscovery profile management", "[midi][ci]") {
    CiDiscovery ci;

    ProfileId profile{0x01, 0x02, 0x01, 0x00, 0x00};
    ci.add_profile({profile, false, 0});

    REQUIRE(ci.profiles().size() == 1);
    REQUIRE_FALSE(ci.profiles()[0].enabled);

    ci.enable_profile(profile);
    REQUIRE(ci.profiles()[0].enabled);

    ci.disable_profile(profile);
    REQUIRE_FALSE(ci.profiles()[0].enabled);
}

TEST_CASE("CiDiscovery profile callback", "[midi][ci]") {
    CiDiscovery ci;
    ProfileId profile{0x01, 0x01, 0x00, 0x00, 0x00};
    ci.add_profile({profile, false, 0});

    bool callback_fired = false;
    ci.on_profile_changed = [&](const ProfileId&, bool) { callback_fired = true; };

    ci.enable_profile(profile);
    REQUIRE(callback_fired);
}

TEST_CASE("CiDiscovery property exchange", "[midi][ci]") {
    CiDiscovery ci;

    ci.set_property("DeviceName", "PulpSynth");
    auto val = ci.get_property("DeviceName");
    REQUIRE(val.has_value());
    REQUIRE(*val == "PulpSynth");

    REQUIRE_FALSE(ci.get_property("nonexistent").has_value());
}

TEST_CASE("CiDiscovery profile inquiry response", "[midi][ci]") {
    CiDiscovery ci;
    ProfileId p1{0x01, 0x01, 0x00, 0x00, 0x00};
    ProfileId p2{0x02, 0x01, 0x00, 0x00, 0x00};
    ci.add_profile({p1, true, 0});
    ci.add_profile({p2, false, 0});

    auto inquiry = ci.create_profile_inquiry(ci.local_muid());
    // Process the inquiry on the same device (self-test)
    auto reply = ci.process_message(inquiry.data(), inquiry.size());
    // Profile inquiry is type 0x24, we handle it
    REQUIRE_FALSE(reply.empty());
}

TEST_CASE("CiDiscovery profile reply echoes inquirer MUID", "[midi][ci]") {
    // Regression: handle_profile_inquiry previously ignored data/size and
    // wrote only our source MUID into the reply, dropping the inquirer's
    // destination field. Multi-device CI buses couldn't route our reply.
    CiDiscovery responder;
    ProfileId p{0x11, 0x01, 0x00, 0x00, 0x00};
    responder.add_profile({p, true, 0});

    // Synthesize a profile inquiry from a distinct peer MUID.
    MUID peer_muid{0x01234567};
    std::vector<uint8_t> inquiry;
    inquiry.push_back(0xF0);
    inquiry.push_back(0x7E);
    inquiry.push_back(0x7F);
    inquiry.push_back(0x0D);
    inquiry.push_back(static_cast<uint8_t>(CiMessageType::ProfileInquiry));
    inquiry.push_back(responder.device_info().ci_version);
    // Source = peer
    inquiry.push_back(peer_muid.value & 0x7F);
    inquiry.push_back((peer_muid.value >> 7) & 0x7F);
    inquiry.push_back((peer_muid.value >> 14) & 0x7F);
    inquiry.push_back((peer_muid.value >> 21) & 0x7F);
    // Destination = responder
    MUID resp_muid = responder.local_muid();
    inquiry.push_back(resp_muid.value & 0x7F);
    inquiry.push_back((resp_muid.value >> 7) & 0x7F);
    inquiry.push_back((resp_muid.value >> 14) & 0x7F);
    inquiry.push_back((resp_muid.value >> 21) & 0x7F);
    inquiry.push_back(0xF7);

    auto reply = responder.process_message(inquiry.data(), inquiry.size());
    REQUIRE(reply.size() >= 14);

    // Reply header: F0 7E 7F 0D ProfileReply version src(4) dst(4)
    REQUIRE(reply[0] == 0xF0);
    REQUIRE(reply[4] == static_cast<uint8_t>(CiMessageType::ProfileReply));
    // Source MUID = responder (bytes 6..9)
    uint32_t src =  (uint32_t)reply[6]
                 | ((uint32_t)reply[7]  << 7)
                 | ((uint32_t)reply[8]  << 14)
                 | ((uint32_t)reply[9]  << 21);
    REQUIRE(src == resp_muid.value);
    // Destination MUID = peer (bytes 10..13) — this is the regression assertion.
    uint32_t dst =  (uint32_t)reply[10]
                 | ((uint32_t)reply[11] << 7)
                 | ((uint32_t)reply[12] << 14)
                 | ((uint32_t)reply[13] << 21);
    REQUIRE(dst == peer_muid.value);
}
