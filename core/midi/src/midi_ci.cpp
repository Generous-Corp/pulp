#include <pulp/midi/midi_ci.hpp>
#include <random>
#include <cstring>
#include <algorithm>
#include <limits>

namespace pulp::midi {

// ── MUID ────────────────────────────────────────────────────────────────

MUID MUID::generate() {
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist(1, 0x0FFFFFFE);
    return {dist(rng)};
}

// ── CI Message encoding helpers ─────────────────────────────────────────

static void write_muid(std::vector<uint8_t>& buf, MUID muid) {
    buf.push_back(muid.value & 0x7F);
    buf.push_back((muid.value >> 7) & 0x7F);
    buf.push_back((muid.value >> 14) & 0x7F);
    buf.push_back((muid.value >> 21) & 0x7F);
}

static MUID read_muid(const uint8_t* data) {
    return {static_cast<uint32_t>(data[0])
          | (static_cast<uint32_t>(data[1]) << 7)
          | (static_cast<uint32_t>(data[2]) << 14)
          | (static_cast<uint32_t>(data[3]) << 21)};
}

// ── CiDiscovery ─────────────────────────────────────────────────────────

CiDiscovery::CiDiscovery() {
    local_info_.muid = MUID::generate();
}

std::vector<uint8_t> CiDiscovery::create_discovery_inquiry() const {
    std::vector<uint8_t> msg;
    // Universal SysEx header: F0 7E <device> 0D <sub-id> ...
    msg.push_back(0xF0);
    msg.push_back(0x7E);
    msg.push_back(0x7F);  // All devices
    msg.push_back(0x0D);  // CI sub-ID #1
    msg.push_back(static_cast<uint8_t>(CiMessageType::DiscoveryInquiry));

    // CI version
    msg.push_back(local_info_.ci_version);

    // Source MUID
    write_muid(msg, local_info_.muid);

    // Destination MUID (broadcast)
    write_muid(msg, MUID::broadcast());

    // Manufacturer
    msg.push_back(local_info_.manufacturer_id & 0x7F);
    msg.push_back((local_info_.manufacturer_id >> 7) & 0x7F);
    msg.push_back((local_info_.manufacturer_id >> 14) & 0x7F);

    // Family, Model
    msg.push_back(local_info_.family_id & 0x7F);
    msg.push_back((local_info_.family_id >> 7) & 0x7F);
    msg.push_back(local_info_.model_id & 0x7F);
    msg.push_back((local_info_.model_id >> 7) & 0x7F);

    // Software version (4 bytes)
    msg.push_back(local_info_.software_version & 0x7F);
    msg.push_back((local_info_.software_version >> 7) & 0x7F);
    msg.push_back((local_info_.software_version >> 14) & 0x7F);
    msg.push_back((local_info_.software_version >> 21) & 0x7F);

    // CI category supported
    msg.push_back(0x07);  // Profile + Property + Process

    // Max SysEx size (4 bytes LE)
    uint32_t max_size = local_info_.max_sysex_size;
    msg.push_back(max_size & 0x7F);
    msg.push_back((max_size >> 7) & 0x7F);
    msg.push_back((max_size >> 14) & 0x7F);
    msg.push_back((max_size >> 21) & 0x7F);

    msg.push_back(0xF7);  // SysEx end
    return msg;
}

std::vector<uint8_t> CiDiscovery::create_profile_inquiry(MUID destination) const {
    std::vector<uint8_t> msg;
    msg.push_back(0xF0);
    msg.push_back(0x7E);
    msg.push_back(0x7F);
    msg.push_back(0x0D);
    msg.push_back(static_cast<uint8_t>(CiMessageType::ProfileInquiry));
    msg.push_back(local_info_.ci_version);
    write_muid(msg, local_info_.muid);
    write_muid(msg, destination);
    msg.push_back(0xF7);
    return msg;
}

std::vector<uint8_t> CiDiscovery::process_message(const uint8_t* data, size_t size) {
    // Minimum CI message: F0 7E device 0D sub-id version source(4) dest(4) ... F7
    if (data == nullptr) return {};
    if (size < 14) return {};
    if (data[0] != 0xF0 || data[1] != 0x7E || data[3] != 0x0D) return {};

    auto type = static_cast<CiMessageType>(data[4]);

    switch (type) {
        case CiMessageType::DiscoveryInquiry:
            return handle_discovery(data, size);
        case CiMessageType::DiscoveryReply: {
            // Store discovered device
            if (size >= 30) {
                MUID dest = read_muid(data + 10);
                if (!dest.is_broadcast() && !(dest == local_info_.muid)) {
                    return {};
                }

                CiDeviceInfo info;
                info.muid = read_muid(data + 6);
                info.ci_version = data[5];
                info.manufacturer_id = static_cast<uint32_t>(data[14])
                    | (static_cast<uint32_t>(data[15]) << 7)
                    | (static_cast<uint32_t>(data[16]) << 14);
                info.family_id = static_cast<uint16_t>(data[17])
                    | (static_cast<uint16_t>(data[18]) << 7);
                info.model_id = static_cast<uint16_t>(data[19])
                    | (static_cast<uint16_t>(data[20]) << 7);
                info.software_version = static_cast<uint32_t>(data[21])
                    | (static_cast<uint32_t>(data[22]) << 7)
                    | (static_cast<uint32_t>(data[23]) << 14)
                    | (static_cast<uint32_t>(data[24]) << 21);
                uint32_t max_sysex_size =
                    static_cast<uint32_t>(data[26])
                    | (static_cast<uint32_t>(data[27]) << 7)
                    | (static_cast<uint32_t>(data[28]) << 14)
                    | (static_cast<uint32_t>(data[29]) << 21);
                info.max_sysex_size = static_cast<uint8_t>(
                    std::min<uint32_t>(max_sysex_size,
                                       std::numeric_limits<uint8_t>::max()));
                discovered_.push_back(info);
                if (on_device_discovered) on_device_discovered(info);
            }
            return {};
        }
        case CiMessageType::ProfileInquiry:
            return handle_profile_inquiry(data, size);
        default:
            return {};
    }
}

std::vector<uint8_t> CiDiscovery::handle_discovery(const uint8_t* data, size_t size) {
    if (size < 14) return {};

    MUID source = read_muid(data + 6);
    MUID dest = read_muid(data + 10);

    // Only respond to broadcast or messages addressed to us
    if (!dest.is_broadcast() && !(dest == local_info_.muid)) return {};

    // Build discovery reply
    std::vector<uint8_t> reply;
    reply.push_back(0xF0);
    reply.push_back(0x7E);
    reply.push_back(0x7F);
    reply.push_back(0x0D);
    reply.push_back(static_cast<uint8_t>(CiMessageType::DiscoveryReply));
    reply.push_back(local_info_.ci_version);
    write_muid(reply, local_info_.muid);
    write_muid(reply, source);

    // Manufacturer, Family, Model, Version
    reply.push_back(local_info_.manufacturer_id & 0x7F);
    reply.push_back((local_info_.manufacturer_id >> 7) & 0x7F);
    reply.push_back((local_info_.manufacturer_id >> 14) & 0x7F);
    reply.push_back(local_info_.family_id & 0x7F);
    reply.push_back((local_info_.family_id >> 7) & 0x7F);
    reply.push_back(local_info_.model_id & 0x7F);
    reply.push_back((local_info_.model_id >> 7) & 0x7F);
    reply.push_back(local_info_.software_version & 0x7F);
    reply.push_back((local_info_.software_version >> 7) & 0x7F);
    reply.push_back((local_info_.software_version >> 14) & 0x7F);
    reply.push_back((local_info_.software_version >> 21) & 0x7F);
    reply.push_back(0x07);

    uint32_t max_size = local_info_.max_sysex_size;
    reply.push_back(max_size & 0x7F);
    reply.push_back((max_size >> 7) & 0x7F);
    reply.push_back((max_size >> 14) & 0x7F);
    reply.push_back((max_size >> 21) & 0x7F);

    reply.push_back(0xF7);
    return reply;
}

std::vector<uint8_t> CiDiscovery::handle_profile_inquiry(const uint8_t* data, size_t size) {
    // ProfileInquiry layout: F0 7E dev 0D sub-id version src(4) dst(4) F7.
    // The reply must echo the inquirer's MUID back as destination so the
    // peer can identify the response. Previous implementation ignored
    // `data`/`size` and dropped the destination field from the reply,
    // leaving multi-device CI buses unable to route our profile reply.
    MUID inquirer = MUID::broadcast();
    if (size >= 14) {
        inquirer = read_muid(data + 6);
        MUID destination = read_muid(data + 10);
        if (!destination.is_broadcast() && !(destination == local_info_.muid)) {
            return {};
        }
    }

    std::vector<uint8_t> reply;
    reply.push_back(0xF0);
    reply.push_back(0x7E);
    reply.push_back(0x7F);
    reply.push_back(0x0D);
    reply.push_back(static_cast<uint8_t>(CiMessageType::ProfileReply));
    reply.push_back(local_info_.ci_version);
    write_muid(reply, local_info_.muid);
    write_muid(reply, inquirer);
    // Count of enabled profiles
    uint16_t enabled_count = 0;
    for (auto& p : profiles_) if (p.enabled) enabled_count++;
    reply.push_back(enabled_count & 0x7F);
    reply.push_back((enabled_count >> 7) & 0x7F);

    for (auto& p : profiles_) {
        if (!p.enabled) continue;
        reply.push_back(p.id.bank);
        reply.push_back(p.id.number);
        reply.push_back(p.id.version);
        reply.push_back(p.id.level);
        reply.push_back(p.id.reserved);
    }

    // Count of disabled profiles
    uint16_t disabled_count = static_cast<uint16_t>(profiles_.size()) - enabled_count;
    reply.push_back(disabled_count & 0x7F);
    reply.push_back((disabled_count >> 7) & 0x7F);

    for (auto& p : profiles_) {
        if (p.enabled) continue;
        reply.push_back(p.id.bank);
        reply.push_back(p.id.number);
        reply.push_back(p.id.version);
        reply.push_back(p.id.level);
        reply.push_back(p.id.reserved);
    }

    reply.push_back(0xF7);
    return reply;
}

bool CiDiscovery::enable_profile(const ProfileId& id) {
    for (auto& p : profiles_) {
        if (p.id == id) {
            p.enabled = true;
            if (on_profile_changed) on_profile_changed(id, true);
            return true;
        }
    }
    return false;
}

bool CiDiscovery::disable_profile(const ProfileId& id) {
    for (auto& p : profiles_) {
        if (p.id == id) {
            p.enabled = false;
            if (on_profile_changed) on_profile_changed(id, false);
            return true;
        }
    }
    return false;
}

void CiDiscovery::set_property(std::string_view resource, std::string_view value) {
    properties_[std::string(resource)] = std::string(value);
}

std::optional<std::string> CiDiscovery::get_property(std::string_view resource) const {
    auto it = properties_.find(resource);
    if (it != properties_.end()) return it->second;
    return std::nullopt;
}

}  // namespace pulp::midi
