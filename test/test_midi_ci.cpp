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
