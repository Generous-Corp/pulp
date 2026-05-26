#pragma once

// MIDI-CI (Capability Inquiry) — MIDI 2.0 device discovery, profiles, property exchange.
// Operates over UMP (Universal MIDI Packets) for CI message encoding/decoding.

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <cstdint>
#include <optional>

namespace pulp::midi {

class PeSubscriptionManager;  // defined in midi_ci_pe.hpp
struct PeSubscription;

/// MIDI-CI message types (sub-IDs)
enum class CiMessageType : uint8_t {
    DiscoveryInquiry     = 0x70,
    DiscoveryReply       = 0x71,
    InvalidateMUID       = 0x7E,
    ProfileInquiry       = 0x24,
    ProfileReply         = 0x25,
    ProfileEnable        = 0x26,
    ProfileDisable       = 0x27,
    PropertyGetInquiry   = 0x34,
    PropertyGetReply     = 0x35,
    PropertySetInquiry   = 0x36,
    PropertySetReply     = 0x37,
    PropertySubscribeInquiry = 0x38,
    PropertySubscribeReply   = 0x39,
    PropertyNotify           = 0x3F,
    ProcessInquiry       = 0x40,
    ProcessReply         = 0x41
};

/// MIDI-CI MUID (Message Unique Identifier) — 28-bit random ID
struct MUID {
    uint32_t value = 0;

    static MUID generate();
    static MUID broadcast() { return {0x0FFFFFFF}; }
    bool is_broadcast() const { return value == 0x0FFFFFFF; }
    bool operator==(const MUID& o) const { return value == o.value; }
};

/// Notify callback — fires when a Subscribe peer should be notified of a
/// resource change. `subscriber_muid` is the bound peer; `header_json`
/// and `payload` echo the Notify PE message body. The transport layer
/// (the caller of CiDiscovery) is responsible for actually building +
/// sending the Notify wire bytes — this callback just tells you who.
using PeNotifyCallback = std::function<void(MUID subscriber_muid,
                                            std::string_view resource,
                                            std::string_view header_json,
                                            const std::vector<uint8_t>& payload)>;

/// CI device identity (from discovery)
struct CiDeviceInfo {
    MUID muid;
    uint32_t manufacturer_id = 0;  // 3-byte SysEx ID
    uint16_t family_id = 0;
    uint16_t model_id = 0;
    uint32_t software_version = 0;
    uint8_t ci_version = 2;        // CI version supported
    uint8_t max_sysex_size = 128;
};

/// CI Profile identifier (5 bytes)
struct ProfileId {
    uint8_t bank = 0;
    uint8_t number = 0;
    uint8_t version = 0;
    uint8_t level = 0;
    uint8_t reserved = 0;

    bool operator==(const ProfileId& o) const {
        return bank == o.bank && number == o.number && version == o.version && level == o.level;
    }
};

/// CI Profile state
struct ProfileState {
    ProfileId id;
    bool enabled = false;
    uint16_t channel_count = 0;  // 0 = group-wide
};

/// MIDI-CI Discovery manager — handles CI message exchange
class CiDiscovery {
public:
    CiDiscovery();
    ~CiDiscovery();

    /// Set this device's identity info
    void set_device_info(const CiDeviceInfo& info) { local_info_ = info; }
    const CiDeviceInfo& device_info() const { return local_info_; }

    /// Get our MUID
    MUID local_muid() const { return local_info_.muid; }

    /// Process an incoming CI message (raw SysEx bytes).
    /// Returns a response message to send, or empty if none.
    std::vector<uint8_t> process_message(const uint8_t* data, size_t size);

    /// Create a discovery inquiry message
    std::vector<uint8_t> create_discovery_inquiry() const;

    /// Create a profile inquiry message for a specific destination
    std::vector<uint8_t> create_profile_inquiry(MUID destination) const;

    /// Known remote devices (populated via discovery replies)
    const std::vector<CiDeviceInfo>& discovered_devices() const { return discovered_; }

    /// Registered profiles
    void add_profile(const ProfileState& profile) { profiles_.push_back(profile); }
    const std::vector<ProfileState>& profiles() const { return profiles_; }

    /// Enable/disable a profile
    bool enable_profile(const ProfileId& id);
    bool disable_profile(const ProfileId& id);

    /// Property storage (simple key-value for CI property exchange)
    void set_property(std::string_view resource, std::string_view value);
    std::optional<std::string> get_property(std::string_view resource) const;

    /// Callbacks
    std::function<void(const CiDeviceInfo&)> on_device_discovered;
    std::function<void(const ProfileId&, bool enabled)> on_profile_changed;

    /// Fires when a Subscribe peer should receive a Notify. Set this
    /// to wire the manager's fan-out to the actual MIDI transport.
    /// macOS plan §8.4 — Subscribe/Notify dispatcher.
    PeNotifyCallback on_pe_notify;

    /// Direct access to the subscription registry for callers that
    /// want to inspect / mutate it outside the wire-message handlers
    /// (test fixtures, programmatic subscription, etc.). Lazily
    /// constructed on first use.
    PeSubscriptionManager& subscription_manager();
    const PeSubscriptionManager& subscription_manager() const;

    /// Programmatic Notify entry point — used by application code that
    /// has changed a resource and wants every subscriber to be told.
    /// Looks up subscribers via the manager and invokes `on_pe_notify`
    /// once per binding. Returns the number of subscribers notified.
    std::size_t notify(std::string_view resource,
                       std::string_view header_json,
                       const std::vector<uint8_t>& payload);

private:
    CiDeviceInfo local_info_;
    std::vector<CiDeviceInfo> discovered_;
    std::vector<ProfileState> profiles_;
    std::map<std::string, std::string, std::less<>> properties_;
    mutable std::unique_ptr<PeSubscriptionManager> sub_mgr_;

    std::vector<uint8_t> handle_discovery(const uint8_t* data, size_t size);
    std::vector<uint8_t> handle_profile_inquiry(const uint8_t* data, size_t size);
    std::vector<uint8_t> handle_subscribe(const uint8_t* data, size_t size);
    void handle_notify(const uint8_t* data, size_t size);
};

}  // namespace pulp::midi
