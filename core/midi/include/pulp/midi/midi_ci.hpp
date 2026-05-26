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

/// MIDI-CI Discovery manager — handles CI message exchange.
///
/// RT-safety contract (audited 2026-05-26 for plan item 8.4):
/// Every public method is annotated below. As a rule, CI is a UI/main
/// thread surface (SysEx is slow, JSON parsing allocates, subscription
/// state mutates). Call from the audio thread only methods explicitly
/// tagged RT-safe.
class CiDiscovery {
public:
    /// RT-safe (constructs an MUID via std::mt19937; the
    /// random_device seed is one-shot at construction). Construct
    /// from a non-RT thread.
    CiDiscovery();
    /// RT-safe. Destructor frees the optional PeSubscriptionManager
    /// (heap free) — not RT-safe to call from the audio thread.
    ~CiDiscovery();

    /// RT-safe. Trivial copy of the small CiDeviceInfo struct.
    void set_device_info(const CiDeviceInfo& info) { local_info_ = info; }
    /// RT-safe. Returns a reference; no allocation.
    const CiDeviceInfo& device_info() const { return local_info_; }

    /// RT-safe. Returns a 4-byte struct by value.
    MUID local_muid() const { return local_info_.muid; }

    /// NOT RT-safe — allocates a response vector and dispatches into
    /// `handle_*` methods that allocate. Call from the MIDI / main
    /// thread, never from the audio callback. Returns an empty vector
    /// when no reply is warranted.
    std::vector<uint8_t> process_message(const uint8_t* data, size_t size);

    /// NOT RT-safe — allocates and returns a SysEx wire buffer.
    std::vector<uint8_t> create_discovery_inquiry() const;

    /// NOT RT-safe — allocates and returns a SysEx wire buffer.
    std::vector<uint8_t> create_profile_inquiry(MUID destination) const;

    /// RT-safe. Returns a reference to the internal vector; no allocation.
    /// The vector itself may grow on a non-RT thread via process_message();
    /// readers should treat this as a snapshot, not a live view.
    const std::vector<CiDeviceInfo>& discovered_devices() const { return discovered_; }

    /// NOT RT-safe — std::vector::push_back may reallocate.
    void add_profile(const ProfileState& profile) { profiles_.push_back(profile); }
    /// RT-safe. Returns a reference; no allocation.
    const std::vector<ProfileState>& profiles() const { return profiles_; }

    /// NOT RT-safe — invokes the on_profile_changed callback which may
    /// allocate. The internal lookup is O(n) but allocation-free.
    bool enable_profile(const ProfileId& id);
    /// NOT RT-safe — see `enable_profile`.
    bool disable_profile(const ProfileId& id);

    /// NOT RT-safe — `std::map::insert` and string construction both
    /// allocate. Call from the main / property-mutation thread.
    void set_property(std::string_view resource, std::string_view value);
    /// RT-safe with caveats: the lookup uses heterogeneous `std::less<>`
    /// so no string allocation occurs for the search key. HOWEVER the
    /// return path constructs a std::string from the stored value, which
    /// allocates. **Do not call from the audio thread.** Use
    /// `subscription_manager().subscribers_of()` only on a non-RT thread.
    std::optional<std::string> get_property(std::string_view resource) const;

    /// Callbacks
    std::function<void(const CiDeviceInfo&)> on_device_discovered;
    std::function<void(const ProfileId&, bool enabled)> on_profile_changed;

    /// Fires when a Subscribe peer should receive a Notify. Set this
    /// to wire the manager's fan-out to the actual MIDI transport.
    /// macOS plan §8.4 — Subscribe/Notify dispatcher.
    ///
    /// The callback itself is invoked from `notify()` / `handle_notify()`
    /// and is therefore on the same thread as those calls — NOT the
    /// audio thread.
    PeNotifyCallback on_pe_notify;

    /// NOT RT-safe — lazily allocates the underlying
    /// `PeSubscriptionManager` (heap allocation through
    /// `std::make_unique`). Construct or warm up the manager from the
    /// main thread before any RT thread observes it.
    PeSubscriptionManager& subscription_manager();
    /// NOT RT-safe — see non-const overload. The const overload performs
    /// the same lazy allocation because `sub_mgr_` is `mutable`.
    const PeSubscriptionManager& subscription_manager() const;

    /// NOT RT-safe — invokes `subscription_manager()` (potentially lazy
    /// allocation) and `subscribers_of()` (allocates a result vector),
    /// then invokes the on_pe_notify callback once per binding (callback
    /// body unknown to us). Drive Notify from the same non-RT thread that
    /// produced the resource change.
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
