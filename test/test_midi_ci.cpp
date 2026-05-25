#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/midi_ci.hpp>

#include <cstddef>

using namespace pulp::midi;

namespace {

void append_muid(std::vector<uint8_t>& msg, MUID muid) {
    msg.push_back(muid.value & 0x7F);
    msg.push_back((muid.value >> 7) & 0x7F);
    msg.push_back((muid.value >> 14) & 0x7F);
    msg.push_back((muid.value >> 21) & 0x7F);
}

uint32_t read_muid_at(const std::vector<uint8_t>& msg, std::size_t offset) {
    return static_cast<uint32_t>(msg[offset])
        | (static_cast<uint32_t>(msg[offset + 1]) << 7)
        | (static_cast<uint32_t>(msg[offset + 2]) << 14)
        | (static_cast<uint32_t>(msg[offset + 3]) << 21);
}

uint32_t read_uint7_at(const std::vector<uint8_t>& msg, std::size_t offset, std::size_t bytes) {
    uint32_t value = 0;
    for (std::size_t i = 0; i < bytes; ++i)
        value |= static_cast<uint32_t>(msg[offset + i]) << (7 * i);
    return value;
}

void append_uint7(std::vector<uint8_t>& msg, uint32_t value, std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; ++i)
        msg.push_back((value >> (7 * i)) & 0x7F);
}

std::vector<uint8_t> make_ci_header(CiMessageType type, MUID source, MUID destination) {
    std::vector<uint8_t> msg{0xF0, 0x7E, 0x7F, 0x0D,
                             static_cast<uint8_t>(type), 0x02};
    append_muid(msg, source);
    append_muid(msg, destination);
    return msg;
}

std::vector<uint8_t> make_discovery_reply(MUID source,
                                          MUID destination,
                                          const CiDeviceInfo& info) {
    auto msg = make_ci_header(CiMessageType::DiscoveryReply, source, destination);
    append_uint7(msg, info.manufacturer_id, 3);
    append_uint7(msg, info.family_id, 2);
    append_uint7(msg, info.model_id, 2);
    append_uint7(msg, info.software_version, 4);
    msg.push_back(0x07);
    append_uint7(msg, info.max_sysex_size, 4);
    msg.push_back(0xF7);
    return msg;
}

} // namespace

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

TEST_CASE("CiDiscovery discovery inquiry encodes local identity fields",
          "[midi][ci][issue-645]") {
    CiDiscovery ci;
    CiDeviceInfo info;
    info.muid = MUID{0x01234567};
    info.manufacturer_id = 0x00123456;
    info.family_id = 0x2345;
    info.model_id = 0x3456;
    info.software_version = 0x01234567;
    info.ci_version = 3;
    info.max_sysex_size = 96;
    ci.set_device_info(info);

    auto msg = ci.create_discovery_inquiry();

    REQUIRE(msg.size() == 31);
    REQUIRE(msg[4] == static_cast<uint8_t>(CiMessageType::DiscoveryInquiry));
    REQUIRE(msg[5] == 3);
    REQUIRE(read_muid_at(msg, 6) == info.muid.value);
    REQUIRE(read_muid_at(msg, 10) == MUID::broadcast().value);
    REQUIRE(read_uint7_at(msg, 14, 3) == info.manufacturer_id);
    REQUIRE(read_uint7_at(msg, 17, 2) == info.family_id);
    REQUIRE(read_uint7_at(msg, 19, 2) == info.model_id);
    REQUIRE(read_uint7_at(msg, 21, 4) == info.software_version);
    REQUIRE(msg[25] == 0x07);
    REQUIRE(read_uint7_at(msg, 26, 4) == info.max_sysex_size);
}

TEST_CASE("CiDiscovery profile inquiry encodes destination",
          "[midi][ci][issue-645]") {
    CiDiscovery ci;
    MUID destination{0x01234567};

    auto msg = ci.create_profile_inquiry(destination);

    REQUIRE(msg.size() == 15);
    REQUIRE(msg.front() == 0xF0);
    REQUIRE(msg.back() == 0xF7);
    REQUIRE(msg[4] == static_cast<uint8_t>(CiMessageType::ProfileInquiry));
    REQUIRE(read_muid_at(msg, 6) == ci.local_muid().value);
    REQUIRE(read_muid_at(msg, 10) == destination.value);
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

TEST_CASE("CiDiscovery discovery reply addresses source and encodes responder identity",
          "[midi][ci][issue-645]") {
    CiDiscovery responder;
    CiDeviceInfo info;
    info.muid = MUID{0x0002468A};
    info.manufacturer_id = 0x00010203;
    info.family_id = 0x0203;
    info.model_id = 0x0304;
    info.software_version = 0x00040506;
    info.ci_version = 4;
    info.max_sysex_size = 120;
    responder.set_device_info(info);

    MUID peer{0x00013579};
    auto inquiry = make_ci_header(CiMessageType::DiscoveryInquiry,
                                  peer, MUID::broadcast());
    inquiry.push_back(0xF7);

    auto reply = responder.process_message(inquiry.data(), inquiry.size());

    REQUIRE(reply.size() == 31);
    REQUIRE(reply[4] == static_cast<uint8_t>(CiMessageType::DiscoveryReply));
    REQUIRE(reply[5] == info.ci_version);
    REQUIRE(read_muid_at(reply, 6) == info.muid.value);
    REQUIRE(read_muid_at(reply, 10) == peer.value);
    REQUIRE(read_uint7_at(reply, 14, 3) == info.manufacturer_id);
    REQUIRE(read_uint7_at(reply, 17, 2) == info.family_id);
    REQUIRE(read_uint7_at(reply, 19, 2) == info.model_id);
    REQUIRE(read_uint7_at(reply, 21, 4) == info.software_version);
    REQUIRE(read_uint7_at(reply, 26, 4) == info.max_sysex_size);
    REQUIRE(reply.back() == 0xF7);
}

TEST_CASE("CiDiscovery ignores malformed and unhandled messages",
          "[midi][ci][issue-645]") {
    CiDiscovery ci;
    std::vector<uint8_t> too_short{0xF0, 0x7E, 0x7F};
    std::vector<uint8_t> wrong_start{0x00, 0x7E, 0x7F, 0x0D,
                                     static_cast<uint8_t>(CiMessageType::DiscoveryInquiry),
                                     0x02, 0, 0, 0, 0, 0, 0, 0, 0};
    auto unknown = make_ci_header(static_cast<CiMessageType>(0x01),
                                  MUID{0x01020304}, MUID::broadcast());
    unknown.push_back(0xF7);

    REQUIRE(ci.process_message(too_short.data(), too_short.size()).empty());
    REQUIRE(ci.process_message(wrong_start.data(), wrong_start.size()).empty());
    REQUIRE(ci.process_message(unknown.data(), unknown.size()).empty());
}

TEST_CASE("CiDiscovery ignores null message buffers",
          "[midi][ci][codecov]") {
    CiDiscovery ci;

    REQUIRE(ci.process_message(nullptr, 0).empty());
    REQUIRE(ci.process_message(nullptr, 31).empty());
}

TEST_CASE("CiDiscovery rejects wrong universal sysex headers",
          "[midi][ci][codecov]") {
    CiDiscovery ci;
    auto wrong_universal_id = make_ci_header(CiMessageType::DiscoveryInquiry,
                                             MUID{0x00011111}, MUID::broadcast());
    wrong_universal_id[1] = 0x7D;
    wrong_universal_id.push_back(0xF7);

    auto wrong_ci_subid = make_ci_header(CiMessageType::DiscoveryInquiry,
                                         MUID{0x00011111}, MUID::broadcast());
    wrong_ci_subid[3] = 0x0E;
    wrong_ci_subid.push_back(0xF7);

    REQUIRE(ci.process_message(wrong_universal_id.data(),
                               wrong_universal_id.size()).empty());
    REQUIRE(ci.process_message(wrong_ci_subid.data(),
                               wrong_ci_subid.size()).empty());
}

TEST_CASE("CiDiscovery ignores undersized discovery replies",
          "[midi][ci][codecov]") {
    CiDiscovery ci;
    int callbacks = 0;
    ci.on_device_discovered = [&](const CiDeviceInfo&) { ++callbacks; };

    auto reply = make_ci_header(CiMessageType::DiscoveryReply,
                                MUID{0x00022222}, ci.local_muid());
    reply.push_back(0xF7);

    REQUIRE(ci.process_message(reply.data(), reply.size()).empty());
    REQUIRE(ci.discovered_devices().empty());
    REQUIRE(callbacks == 0);
}

TEST_CASE("CiDiscovery ignores inquiries addressed to another MUID",
          "[midi][ci][issue-645]") {
    CiDiscovery responder;
    CiDeviceInfo info = responder.device_info();
    info.muid = MUID{0x00001234};
    responder.set_device_info(info);

    auto inquiry = make_ci_header(CiMessageType::DiscoveryInquiry,
                                  MUID{0x00005678}, MUID{0x00009ABC});
    inquiry.push_back(0xF7);

    REQUIRE(responder.process_message(inquiry.data(), inquiry.size()).empty());
}

TEST_CASE("CiDiscovery responds to inquiry addressed directly to local MUID",
          "[midi][ci][codecov]") {
    CiDiscovery responder;
    CiDeviceInfo info = responder.device_info();
    info.muid = MUID{0x00001234};
    responder.set_device_info(info);

    MUID peer{0x00005678};
    auto inquiry = make_ci_header(CiMessageType::DiscoveryInquiry,
                                  peer, info.muid);
    inquiry.push_back(0xF7);

    auto reply = responder.process_message(inquiry.data(), inquiry.size());

    REQUIRE(reply.size() == 31);
    REQUIRE(reply[4] == static_cast<uint8_t>(CiMessageType::DiscoveryReply));
    REQUIRE(read_muid_at(reply, 6) == info.muid.value);
    REQUIRE(read_muid_at(reply, 10) == peer.value);
}

TEST_CASE("CiDiscovery stores minimal-length discovery replies",
          "[midi][ci][codecov]") {
    CiDiscovery ci;
    std::vector<CiDeviceInfo> callbacks;
    ci.on_device_discovered = [&](const CiDeviceInfo& info) {
        callbacks.push_back(info);
    };

    auto reply = make_ci_header(CiMessageType::DiscoveryReply,
                                MUID{0x00022222}, ci.local_muid());
    while (reply.size() < 30)
        reply.push_back(0);

    REQUIRE(ci.process_message(reply.data(), reply.size()).empty());
    REQUIRE(ci.discovered_devices().size() == 1);
    REQUIRE(ci.discovered_devices()[0].muid.value == 0x00022222);
    REQUIRE(callbacks.size() == 1);
    REQUIRE(callbacks[0].muid.value == 0x00022222);
}

TEST_CASE("CiDiscovery stores discovery replies and fires callbacks",
          "[midi][ci][issue-645]") {
    CiDiscovery inquirer;
    CiDiscovery responder;
    CiDeviceInfo responder_info = responder.device_info();
    responder_info.muid = MUID{0x00123456};
    responder_info.ci_version = 3;
    responder.set_device_info(responder_info);

    std::vector<CiDeviceInfo> callbacks;
    inquirer.on_device_discovered = [&](const CiDeviceInfo& info) {
        callbacks.push_back(info);
    };

    auto inquiry = inquirer.create_discovery_inquiry();
    auto reply = responder.process_message(inquiry.data(), inquiry.size());
    REQUIRE_FALSE(reply.empty());

    auto response = inquirer.process_message(reply.data(), reply.size());

    REQUIRE(response.empty());
    REQUIRE(inquirer.discovered_devices().size() == 1);
    REQUIRE(inquirer.discovered_devices()[0].muid.value == responder_info.muid.value);
    REQUIRE(inquirer.discovered_devices()[0].ci_version == responder_info.ci_version);
    REQUIRE(callbacks.size() == 1);
    REQUIRE(callbacks[0].muid.value == responder_info.muid.value);
}

TEST_CASE("CiDiscovery discovery reply stores identity metadata",
          "[midi][ci][coverage][phase3-routing]") {
    CiDiscovery inquirer;
    CiDeviceInfo local = inquirer.device_info();
    local.muid = MUID{0x00011111};
    inquirer.set_device_info(local);

    CiDeviceInfo remote;
    remote.muid = MUID{0x00022222};
    remote.manufacturer_id = 0x00123456;
    remote.family_id = 0x1234;
    remote.model_id = 0x2345;
    remote.software_version = 0x03456789;
    remote.ci_version = 5;
    remote.max_sysex_size = 96;

    std::vector<CiDeviceInfo> callbacks;
    inquirer.on_device_discovered = [&](const CiDeviceInfo& info) {
        callbacks.push_back(info);
    };

    auto reply = make_discovery_reply(remote.muid, local.muid, remote);
    reply[5] = remote.ci_version;

    REQUIRE(reply.size() == 31);
    REQUIRE(reply.front() == 0xF0);
    REQUIRE(reply.back() == 0xF7);
    REQUIRE(inquirer.process_message(reply.data(), reply.size()).empty());
    REQUIRE(inquirer.discovered_devices().size() == 1);
    const auto& discovered = inquirer.discovered_devices().front();
    REQUIRE(discovered.muid.value == remote.muid.value);
    REQUIRE(discovered.ci_version == remote.ci_version);
    REQUIRE(discovered.manufacturer_id == remote.manufacturer_id);
    REQUIRE(discovered.family_id == remote.family_id);
    REQUIRE(discovered.model_id == remote.model_id);
    REQUIRE(discovered.software_version == remote.software_version);
    REQUIRE(discovered.max_sysex_size == remote.max_sysex_size);
    REQUIRE(callbacks.size() == 1);
    REQUIRE(callbacks.front().manufacturer_id == remote.manufacturer_id);
    REQUIRE(callbacks.front().software_version == remote.software_version);
}

TEST_CASE("CiDiscovery clamps oversized discovery max SysEx metadata",
          "[midi][ci][coverage][phase3-routing]") {
    CiDiscovery inquirer;
    CiDeviceInfo local = inquirer.device_info();
    local.muid = MUID{0x00011111};
    inquirer.set_device_info(local);

    CiDeviceInfo remote;
    remote.muid = MUID{0x00022222};
    auto reply = make_ci_header(CiMessageType::DiscoveryReply,
                                remote.muid, local.muid);
    append_uint7(reply, 0x00010203, 3);
    append_uint7(reply, 0x0102, 2);
    append_uint7(reply, 0x0203, 2);
    append_uint7(reply, 0x00030405, 4);
    reply.push_back(0x07);
    append_uint7(reply, 1024, 4);
    reply.push_back(0xF7);

    REQUIRE(inquirer.process_message(reply.data(), reply.size()).empty());
    REQUIRE(inquirer.discovered_devices().size() == 1);
    REQUIRE(inquirer.discovered_devices().front().max_sysex_size == 255);
}

TEST_CASE("CiDiscovery ignores discovery replies addressed to another MUID",
          "[midi][ci][coverage][phase3-routing]") {
    CiDiscovery inquirer;
    CiDeviceInfo local = inquirer.device_info();
    local.muid = MUID{0x00033333};
    inquirer.set_device_info(local);

    CiDeviceInfo remote;
    remote.muid = MUID{0x00044444};
    remote.manufacturer_id = 0x00010203;
    remote.family_id = 0x0102;
    remote.model_id = 0x0203;
    remote.software_version = 0x00030405;
    remote.max_sysex_size = 64;

    int callbacks = 0;
    inquirer.on_device_discovered = [&](const CiDeviceInfo&) {
        ++callbacks;
    };

    auto misaddressed = make_discovery_reply(remote.muid, MUID{0x00055555}, remote);
    auto broadcast = make_discovery_reply(remote.muid, MUID::broadcast(), remote);

    REQUIRE(inquirer.process_message(misaddressed.data(), misaddressed.size()).empty());
    REQUIRE(inquirer.discovered_devices().empty());
    REQUIRE(callbacks == 0);

    REQUIRE(inquirer.process_message(broadcast.data(), broadcast.size()).empty());
    REQUIRE(inquirer.discovered_devices().size() == 1);
    REQUIRE(inquirer.discovered_devices().front().muid.value == remote.muid.value);
    REQUIRE(callbacks == 1);
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

TEST_CASE("CiDiscovery profile management ignores unknown profiles",
          "[midi][ci][issue-645]") {
    CiDiscovery ci;
    ProfileId known{0x01, 0x02, 0x03, 0x04, 0x00};
    ProfileId unknown{0x10, 0x20, 0x30, 0x40, 0x00};
    ci.add_profile({known, false, 0});

    int callback_count = 0;
    ci.on_profile_changed = [&](const ProfileId&, bool) { ++callback_count; };

    REQUIRE_FALSE(ci.enable_profile(unknown));
    REQUIRE_FALSE(ci.disable_profile(unknown));
    REQUIRE_FALSE(ci.profiles()[0].enabled);
    REQUIRE(callback_count == 0);
}

TEST_CASE("ProfileId equality ignores reserved byte",
          "[midi][ci][codecov]") {
    ProfileId lhs{0x01, 0x02, 0x03, 0x04, 0x00};
    ProfileId same_identity{0x01, 0x02, 0x03, 0x04, 0x7F};
    ProfileId different_level{0x01, 0x02, 0x03, 0x05, 0x00};

    REQUIRE(lhs == same_identity);
    REQUIRE_FALSE(lhs == different_level);
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

TEST_CASE("CiDiscovery profile callback reports enable and disable",
          "[midi][ci][codecov]") {
    CiDiscovery ci;
    ProfileId profile{0x02, 0x03, 0x04, 0x05, 0x00};
    ci.add_profile({profile, false, 4});

    std::vector<bool> states;
    std::vector<ProfileId> ids;
    ci.on_profile_changed = [&](const ProfileId& id, bool enabled) {
        ids.push_back(id);
        states.push_back(enabled);
    };

    REQUIRE(ci.enable_profile(profile));
    REQUIRE(ci.disable_profile(profile));

    REQUIRE(ids.size() == 2);
    REQUIRE(ids[0] == profile);
    REQUIRE(ids[1] == profile);
    REQUIRE(states == std::vector<bool>{true, false});
    REQUIRE(ci.profiles()[0].channel_count == 4);
}

TEST_CASE("CiDiscovery profile enable and disable repeat notifications are explicit",
          "[midi][ci][coverage]") {
    CiDiscovery ci;
    ProfileId profile{0x04, 0x05, 0x06, 0x07, 0x00};
    ci.add_profile({profile, false, 2});

    std::vector<bool> states;
    ci.on_profile_changed = [&](const ProfileId& id, bool enabled) {
        REQUIRE(id == profile);
        states.push_back(enabled);
    };

    REQUIRE(ci.enable_profile(profile));
    REQUIRE(ci.enable_profile(profile));
    REQUIRE(ci.profiles()[0].enabled);

    REQUIRE(ci.disable_profile(profile));
    REQUIRE(ci.disable_profile(profile));
    REQUIRE_FALSE(ci.profiles()[0].enabled);

    REQUIRE(states == std::vector<bool>{true, true, false, false});
}

TEST_CASE("CiDiscovery property exchange", "[midi][ci]") {
    CiDiscovery ci;

    ci.set_property("DeviceName", "PulpSynth");
    auto val = ci.get_property("DeviceName");
    REQUIRE(val.has_value());
    REQUIRE(*val == "PulpSynth");

    ci.set_property("DeviceName", "PulpKeys");
    val = ci.get_property("DeviceName");
    REQUIRE(val.has_value());
    REQUIRE(*val == "PulpKeys");

    REQUIRE_FALSE(ci.get_property("nonexistent").has_value());
}

TEST_CASE("CiDiscovery properties accept empty and overwritten values",
          "[midi][ci][issue-645]") {
    CiDiscovery ci;

    ci.set_property("", "");
    REQUIRE(ci.get_property("").has_value());
    REQUIRE(*ci.get_property("") == "");

    ci.set_property("DeviceName", "First");
    ci.set_property("DeviceName", "Second");
    auto value = ci.get_property("DeviceName");
    REQUIRE(value.has_value());
    REQUIRE(*value == "Second");
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

TEST_CASE("CiDiscovery profile inquiry only responds to local or broadcast MUID",
          "[midi][ci][coverage][phase3-routing]") {
    CiDiscovery responder;
    CiDeviceInfo local = responder.device_info();
    local.muid = MUID{0x00066666};
    responder.set_device_info(local);

    ProfileId profile{0x01, 0x02, 0x03, 0x04, 0x05};
    responder.add_profile({profile, true, 0});

    MUID peer{0x00077777};
    auto misaddressed = make_ci_header(CiMessageType::ProfileInquiry,
                                       peer, MUID{0x00012345});
    misaddressed.push_back(0xF7);
    auto direct = make_ci_header(CiMessageType::ProfileInquiry,
                                 peer, local.muid);
    direct.push_back(0xF7);
    auto broadcast = make_ci_header(CiMessageType::ProfileInquiry,
                                    peer, MUID::broadcast());
    broadcast.push_back(0xF7);

    auto ignored = responder.process_message(misaddressed.data(), misaddressed.size());
    REQUIRE(ignored.empty());

    auto direct_reply = responder.process_message(direct.data(), direct.size());
    REQUIRE_FALSE(direct_reply.empty());
    REQUIRE(direct_reply.size() == 24);
    REQUIRE(direct_reply[4] == static_cast<uint8_t>(CiMessageType::ProfileReply));
    REQUIRE(read_muid_at(direct_reply, 6) == local.muid.value);
    REQUIRE(read_muid_at(direct_reply, 10) == peer.value);
    REQUIRE(direct_reply[14] == 1);
    REQUIRE(direct_reply[15] == 0);
    REQUIRE(direct_reply[16] == profile.bank);
    REQUIRE(direct_reply[20] == profile.reserved);
    REQUIRE(direct_reply[21] == 0);
    REQUIRE(direct_reply[22] == 0);
    REQUIRE(direct_reply.back() == 0xF7);

    auto broadcast_reply = responder.process_message(broadcast.data(), broadcast.size());
    REQUIRE_FALSE(broadcast_reply.empty());
    REQUIRE(broadcast_reply[4] == static_cast<uint8_t>(CiMessageType::ProfileReply));
    REQUIRE(read_muid_at(broadcast_reply, 10) == peer.value);
    REQUIRE(broadcast_reply[14] == 1);
}

TEST_CASE("CiDiscovery profile inquiry with no profiles reports zero counts",
          "[midi][ci][codecov]") {
    CiDiscovery responder;
    MUID peer{0x0002468A};
    auto inquiry = make_ci_header(CiMessageType::ProfileInquiry,
                                  peer, responder.local_muid());
    inquiry.push_back(0xF7);

    auto reply = responder.process_message(inquiry.data(), inquiry.size());

    REQUIRE(reply.size() == 19);
    REQUIRE(reply[4] == static_cast<uint8_t>(CiMessageType::ProfileReply));
    REQUIRE(read_muid_at(reply, 10) == peer.value);
    REQUIRE(reply[14] == 0);
    REQUIRE(reply[15] == 0);
    REQUIRE(reply[16] == 0);
    REQUIRE(reply[17] == 0);
    REQUIRE(reply[18] == 0xF7);
}

TEST_CASE("CiDiscovery profile reply echoes inquirer MUID", "[midi][ci]") {
    // Regression: handle_profile_inquiry previously ignored data/size and
    // wrote only our source MUID into the reply, dropping the inquirer's
    // destination field. Multi-device CI buses couldn't route our reply.
    CiDiscovery responder;
    ProfileId p{0x11, 0x01, 0x00, 0x00, 0x00};
    responder.add_profile({p, true, 0});

    MUID peer_muid{0x01234567};
    MUID resp_muid = responder.local_muid();
    auto inquiry = make_ci_header(CiMessageType::ProfileInquiry,
                                  peer_muid, resp_muid);
    inquiry.push_back(0xF7);

    auto reply = responder.process_message(inquiry.data(), inquiry.size());
    REQUIRE(reply.size() >= 14);

    // Reply header: F0 7E 7F 0D ProfileReply version src(4) dst(4)
    REQUIRE(reply[0] == 0xF0);
    REQUIRE(reply[4] == static_cast<uint8_t>(CiMessageType::ProfileReply));
    // Source MUID = responder (bytes 6..9)
    REQUIRE(read_muid_at(reply, 6) == resp_muid.value);
    // Destination MUID = peer (bytes 10..13) — this is the regression assertion.
    REQUIRE(read_muid_at(reply, 10) == peer_muid.value);
}

TEST_CASE("CiDiscovery profile reply lists enabled and disabled profiles",
          "[midi][ci][issue-645]") {
    CiDiscovery responder;
    ProfileId enabled{0x11, 0x22, 0x01, 0x02, 0x03};
    ProfileId disabled{0x33, 0x44, 0x05, 0x06, 0x07};
    responder.add_profile({enabled, true, 0});
    responder.add_profile({disabled, false, 0});

    MUID peer_muid{0x00013579};
    auto inquiry = make_ci_header(CiMessageType::ProfileInquiry,
                                  peer_muid, responder.local_muid());
    inquiry.push_back(0xF7);

    auto reply = responder.process_message(inquiry.data(), inquiry.size());

    REQUIRE(reply.size() == 29);
    REQUIRE(reply[4] == static_cast<uint8_t>(CiMessageType::ProfileReply));
    REQUIRE(reply[14] == 1);
    REQUIRE(reply[15] == 0);
    REQUIRE(reply[16] == enabled.bank);
    REQUIRE(reply[17] == enabled.number);
    REQUIRE(reply[18] == enabled.version);
    REQUIRE(reply[19] == enabled.level);
    REQUIRE(reply[20] == enabled.reserved);
    REQUIRE(reply[21] == 1);
    REQUIRE(reply[22] == 0);
    REQUIRE(reply[23] == disabled.bank);
    REQUIRE(reply[24] == disabled.number);
    REQUIRE(reply[25] == disabled.version);
    REQUIRE(reply[26] == disabled.level);
    REQUIRE(reply[27] == disabled.reserved);
    REQUIRE(reply[28] == 0xF7);
}

TEST_CASE("CiDiscovery profile reply encodes multi-byte profile counts",
          "[midi][ci][codecov]") {
    CiDiscovery responder;
    for (int i = 0; i < 130; ++i) {
        responder.add_profile({
            ProfileId{0x01, static_cast<uint8_t>(i & 0x7F), 0x00, 0x00, 0x00},
            true,
            0,
        });
    }
    for (int i = 0; i < 129; ++i) {
        responder.add_profile({
            ProfileId{0x02, static_cast<uint8_t>(i & 0x7F), 0x00, 0x00, 0x00},
            false,
            0,
        });
    }

    auto inquiry = make_ci_header(CiMessageType::ProfileInquiry,
                                  MUID{0x00013579}, responder.local_muid());
    inquiry.push_back(0xF7);

    auto reply = responder.process_message(inquiry.data(), inquiry.size());

    constexpr std::size_t enabled_count_offset = 14;
    constexpr std::size_t enabled_profile_bytes = 130 * 5;
    constexpr std::size_t disabled_count_offset =
        enabled_count_offset + 2 + enabled_profile_bytes;

    REQUIRE(reply.size() == disabled_count_offset + 2 + 129 * 5 + 1);
    REQUIRE(reply[enabled_count_offset] == 2);
    REQUIRE(reply[enabled_count_offset + 1] == 1);
    REQUIRE(reply[disabled_count_offset] == 1);
    REQUIRE(reply[disabled_count_offset + 1] == 1);
    REQUIRE(reply.back() == 0xF7);
}
