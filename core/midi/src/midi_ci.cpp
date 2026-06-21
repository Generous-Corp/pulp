#include <pulp/midi/midi_ci.hpp>
#include <pulp/midi/midi_ci_pe.hpp>
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

CiDiscovery::~CiDiscovery() = default;

PeSubscriptionManager& CiDiscovery::subscription_manager() {
    if (!sub_mgr_) sub_mgr_ = std::make_unique<PeSubscriptionManager>();
    return *sub_mgr_;
}

const PeSubscriptionManager& CiDiscovery::subscription_manager() const {
    if (!sub_mgr_) sub_mgr_ = std::make_unique<PeSubscriptionManager>();
    return *sub_mgr_;
}

std::size_t CiDiscovery::notify(std::string_view resource,
                                std::string_view header_json,
                                const std::vector<uint8_t>& payload) {
    auto subscribers = subscription_manager().subscribers_of(resource);
    // Document contract: returns the number of subscribers actually
    // notified. If `on_pe_notify` is not installed, NO notifications
    // are emitted — return 0, not `subscribers.size()`. Callers using
    // the return for delivery accounting / retry would otherwise be
    // told everything succeeded when in fact nothing was sent.
    if (!on_pe_notify) return 0;
    std::size_t delivered = 0;
    for (const auto& sub : subscribers) {
        on_pe_notify(sub.subscriber, resource, header_json, payload);
        ++delivered;
    }
    return delivered;
}

std::vector<uint8_t> CiDiscovery::create_discovery_inquiry() const {
    std::vector<uint8_t> msg;
    // Universal SysEx header: F0 7E <device> 0D <sub-id> ...
    msg.push_back(0xF0);
    msg.push_back(0x7E);
    msg.push_back(0x7F);  // All devices
    msg.push_back(0x0D);  // MIDI-CI sub-ID byte
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
        case CiMessageType::ProfileEnable:
            return handle_profile_added(data, size);
        case CiMessageType::ProfileDisable:
            return handle_profile_removed(data, size);
        case CiMessageType::ProfileDetailsInquiry:
            return handle_profile_details_inquiry(data, size);
        case CiMessageType::ProfileDetailsReply:
            handle_profile_details_reply(data, size);
            return {};
        case CiMessageType::ProfileSpecificData:
            handle_profile_specific_data(data, size);
            return {};
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
        case CiMessageType::PropertySubscribeInquiry:
            return handle_subscribe(data, size);
        case CiMessageType::PropertyNotify:
            handle_notify(data, size);
            return {};
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

// ── PE Subscribe / Notify dispatcher (macOS plan §8.4) ──────────────────
//
// Subscribe Inquiry → register the source MUID against the resource named
// in the PE JSON header → reply with a Subscribe Reply carrying the
// allocated subscription id (echoed back in the same JSON header).
// Notify → look up subscribers by resource and fan out via on_pe_notify.

std::vector<uint8_t> CiDiscovery::handle_subscribe(const uint8_t* data, size_t size) {
    auto chunk = pe_parse_message(data, size);
    if (!chunk) return {};
    // PE message header lives at data[6..9] (source MUID) and data[10..13]
    // (destination MUID). Only respond when addressed to us (or broadcast).
    MUID source = read_muid(data + 6);
    MUID dest = read_muid(data + 10);
    if (!dest.is_broadcast() && !(dest == local_info_.muid)) return {};

    std::string resource, command;
    int status = 200;
    pe_header_parse(chunk->header_json, &resource, &command, &status);
    if (resource.empty()) return {};

    const std::string sub_id = subscription_manager().subscribe(resource, source);

    // Build Subscribe Reply: echo resource + add subscribeId to JSON header.
    // Header form: {"resource":"<r>","command":"<c>","subscribeId":"<id>","status":200}
    std::string reply_header;
    reply_header.reserve(64 + resource.size() + sub_id.size());
    reply_header += "{\"resource\":\"";
    reply_header += resource;
    reply_header += "\",\"subscribeId\":\"";
    reply_header += sub_id;
    reply_header += "\",\"status\":200}";

    return pe_build_message(PeMessageType::SubscribeReply,
                            local_info_.ci_version,
                            local_info_.muid,
                            source,
                            chunk->request_id,
                            reply_header,
                            /*total_chunks=*/1,
                            /*chunk_number=*/1,
                            chunk->payload.data(),
                            chunk->payload.size());
}

void CiDiscovery::handle_notify(const uint8_t* data, size_t size) {
    auto chunk = pe_parse_message(data, size);
    if (!chunk) return;
    // Notify is one-way — server → subscribers. We may receive it as a
    // client (then `on_pe_notify` is the application's hook) or, when
    // routing through a session, as a relay (the fan-out path is
    // `notify()` on the producing side; this handler covers the
    // consumer side).
    if (!on_pe_notify) return;

    // Destination-MUID gate. Without this, on a multi-device MIDI-CI bus
    // (or when sharing a transport with multiple ci-capable peers), a
    // PropertyNotify addressed to another peer would still fire local
    // handlers and apply/surface updates for the wrong session/resource.
    // Other handlers (Subscribe, Discovery, InquireProperties, …) gate
    // identically at data+10.
    MUID source = read_muid(data + 6);
    MUID dest = read_muid(data + 10);
    if (!dest.is_broadcast() && !(dest == local_info_.muid)) return;

    std::string resource, command;
    int status = 200;
    pe_header_parse(chunk->header_json, &resource, &command, &status);
    if (resource.empty()) return;

    // Source MUID identifies who emitted the notification — pass that
    // up so the application can disambiguate multi-publisher topics.
    on_pe_notify(source, resource, chunk->header_json, chunk->payload);
}

// ── Profile Added / Removed / Details / Specific Data (CI 1.2 §7) ──────

static std::vector<uint8_t> ci_envelope(CiMessageType type,
                                        uint8_t ci_version,
                                        MUID source,
                                        MUID destination) {
    std::vector<uint8_t> msg;
    msg.push_back(0xF0);
    msg.push_back(0x7E);
    msg.push_back(0x7F);
    msg.push_back(0x0D);
    msg.push_back(static_cast<uint8_t>(type));
    msg.push_back(ci_version);
    write_muid(msg, source);
    write_muid(msg, destination);
    return msg;
}

static void append_profile_id(std::vector<uint8_t>& msg, const ProfileId& id) {
    msg.push_back(id.bank);
    msg.push_back(id.number);
    msg.push_back(id.version);
    msg.push_back(id.level);
    msg.push_back(id.reserved);
}

static bool read_profile_id(const uint8_t* data, size_t size,
                            size_t offset, ProfileId* out) {
    if (size < offset + 5) return false;
    out->bank     = data[offset + 0];
    out->number   = data[offset + 1];
    out->version  = data[offset + 2];
    out->level    = data[offset + 3];
    out->reserved = data[offset + 4];
    return true;
}

std::vector<uint8_t> CiDiscovery::create_profile_added_report(MUID destination,
                                                              const ProfileId& id) {
    auto msg = ci_envelope(CiMessageType::ProfileEnable, local_info_.ci_version,
                           local_info_.muid, destination);
    append_profile_id(msg, id);
    msg.push_back(0xF7);
    return msg;
}

std::vector<uint8_t> CiDiscovery::create_profile_removed_report(MUID destination,
                                                                const ProfileId& id) {
    auto msg = ci_envelope(CiMessageType::ProfileDisable, local_info_.ci_version,
                           local_info_.muid, destination);
    append_profile_id(msg, id);
    msg.push_back(0xF7);
    return msg;
}

void CiDiscovery::set_profile_details(const ProfileId& id, uint8_t target,
                                      const std::vector<uint8_t>& data) {
    for (auto& d : local_details_) {
        if (d.id == id && d.target == target) {
            d.data = data;
            return;
        }
    }
    local_details_.push_back({id, target, data});
}

std::vector<ProfileDetails> CiDiscovery::profile_details_for(const ProfileId& id) const {
    std::vector<ProfileDetails> out;
    for (auto& d : local_details_) {
        if (d.id == id) out.push_back(d);
    }
    return out;
}

std::vector<uint8_t> CiDiscovery::create_profile_details_inquiry(MUID destination,
                                                                 const ProfileId& id,
                                                                 uint8_t target) const {
    auto msg = ci_envelope(CiMessageType::ProfileDetailsInquiry,
                           local_info_.ci_version, local_info_.muid, destination);
    append_profile_id(msg, id);
    msg.push_back(target);
    msg.push_back(0xF7);
    return msg;
}

std::vector<uint8_t> CiDiscovery::create_profile_specific_data(MUID destination,
                                                               const ProfileId& id,
                                                               const uint8_t* data,
                                                               std::size_t size) const {
    auto msg = ci_envelope(CiMessageType::ProfileSpecificData,
                           local_info_.ci_version, local_info_.muid, destination);
    append_profile_id(msg, id);
    // Length: 2-byte 7-bit LE
    msg.push_back(static_cast<uint8_t>(size & 0x7F));
    msg.push_back(static_cast<uint8_t>((size >> 7) & 0x7F));
    if (data && size > 0) {
        msg.insert(msg.end(), data, data + size);
    }
    msg.push_back(0xF7);
    return msg;
}

std::vector<uint8_t> CiDiscovery::handle_profile_added(const uint8_t* data, size_t size) {
    if (size < 14 + 5 + 1) return {};
    MUID source = read_muid(data + 6);
    MUID dest = read_muid(data + 10);
    if (!dest.is_broadcast() && !(dest == local_info_.muid)) return {};
    ProfileId id;
    if (!read_profile_id(data, size, 14, &id)) return {};
    if (on_profile_added) on_profile_added(source, id);
    return {};
}

std::vector<uint8_t> CiDiscovery::handle_profile_removed(const uint8_t* data, size_t size) {
    if (size < 14 + 5 + 1) return {};
    MUID source = read_muid(data + 6);
    MUID dest = read_muid(data + 10);
    if (!dest.is_broadcast() && !(dest == local_info_.muid)) return {};
    ProfileId id;
    if (!read_profile_id(data, size, 14, &id)) return {};
    if (on_profile_removed) on_profile_removed(source, id);
    return {};
}

std::vector<uint8_t> CiDiscovery::handle_profile_details_inquiry(const uint8_t* data,
                                                                 size_t size) {
    // Layout: envelope(14) profile_id(5) target(1) F7
    if (size < 14 + 5 + 1 + 1) return {};
    MUID source = read_muid(data + 6);
    MUID dest = read_muid(data + 10);
    if (!dest.is_broadcast() && !(dest == local_info_.muid)) return {};
    ProfileId id;
    if (!read_profile_id(data, size, 14, &id)) return {};
    uint8_t target = data[19];

    // Look up our local store for a matching (id, target).
    const ProfileDetails* found = nullptr;
    for (auto& d : local_details_) {
        if (d.id == id && d.target == target) { found = &d; break; }
    }
    if (!found) return {};

    auto reply = ci_envelope(CiMessageType::ProfileDetailsReply,
                             local_info_.ci_version, local_info_.muid, source);
    append_profile_id(reply, id);
    reply.push_back(target);
    // 2-byte 7-bit LE length
    uint16_t len = static_cast<uint16_t>(found->data.size());
    reply.push_back(static_cast<uint8_t>(len & 0x7F));
    reply.push_back(static_cast<uint8_t>((len >> 7) & 0x7F));
    reply.insert(reply.end(), found->data.begin(), found->data.end());
    reply.push_back(0xF7);
    return reply;
}

void CiDiscovery::handle_profile_details_reply(const uint8_t* data, size_t size) {
    if (size < 14 + 5 + 1 + 2 + 1) return;
    MUID source = read_muid(data + 6);
    MUID dest = read_muid(data + 10);
    if (!dest.is_broadcast() && !(dest == local_info_.muid)) return;
    ProfileId id;
    if (!read_profile_id(data, size, 14, &id)) return;
    uint8_t target = data[19];
    uint16_t len = static_cast<uint16_t>(data[20])
                 | (static_cast<uint16_t>(data[21]) << 7);
    if (size < 14 + 5 + 1 + 2 + len + 1) return;

    ProfileDetails d;
    d.id = id;
    d.target = target;
    d.data.assign(data + 22, data + 22 + len);
    discovered_details_.push_back(d);
    if (on_profile_details) on_profile_details(source, d);
}

void CiDiscovery::handle_profile_specific_data(const uint8_t* data, size_t size) {
    if (size < 14 + 5 + 2 + 1) return;
    MUID source = read_muid(data + 6);
    MUID dest = read_muid(data + 10);
    if (!dest.is_broadcast() && !(dest == local_info_.muid)) return;
    ProfileId id;
    if (!read_profile_id(data, size, 14, &id)) return;
    uint16_t len = static_cast<uint16_t>(data[19])
                 | (static_cast<uint16_t>(data[20]) << 7);
    if (size < 14 + 5 + 2 + len + 1) return;

    std::vector<uint8_t> payload(data + 21, data + 21 + len);
    if (on_profile_specific_data) on_profile_specific_data(source, id, payload);
}

}  // namespace pulp::midi
