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
///
/// Note: Pulp uses 0x24 / 0x25 / 0x26 / 0x27 for the Profile Inquiry
/// family (existing contract; preserved for wire compatibility). The
/// CI 1.2 Profile Details / Profile Specific Data / Set Profile On|Off
/// values follow that block at 0x28 / 0x29 / 0x2F / 0x22 / 0x23. New
/// values are appended only — old code paths keep their numeric IDs.
enum class CiMessageType : uint8_t {
    DiscoveryInquiry     = 0x70,
    DiscoveryReply       = 0x71,
    InvalidateMUID       = 0x7E,
    ProfileSetOn         = 0x22,  ///< Set Profile On (CI 1.2 §7.4)
    ProfileSetOff        = 0x23,  ///< Set Profile Off (CI 1.2 §7.5)
    ProfileInquiry       = 0x24,
    ProfileReply         = 0x25,
    ProfileEnable        = 0x26,  ///< Pulp legacy Profile Enabled report
    ProfileDisable       = 0x27,  ///< Pulp legacy Profile Disabled report
    ProfileDetailsInquiry = 0x28, ///< Profile Details Inquiry (CI 1.2 §7.6)
    ProfileDetailsReply   = 0x29, ///< Profile Details Reply (CI 1.2 §7.6)
    ProfileSpecificData   = 0x2F, ///< Profile-Specific Data (CI 1.2 §7.7)
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

/// UMP Function Block identifier (MIDI 2.0 §A) — a Function Block is a
/// logical grouping of UMP groups that share a single set of CI / Profile
/// state. CI 1.2 §3.2.1 says every CI message addresses either a Function
/// Block or a specific UMP group within one. `0x7F` is the broadcast
/// "all function blocks" address; `0x00..=0x1F` enumerate up to 32 blocks
/// per endpoint.
struct FunctionBlockId {
    uint8_t value = 0x7F;  ///< 0x7F = broadcast / "all blocks"

    static FunctionBlockId broadcast() { return {0x7F}; }
    bool is_broadcast() const { return value == 0x7F; }
    bool operator==(const FunctionBlockId& o) const { return value == o.value; }
};

/// Description of a single Function Block as advertised by an endpoint.
/// Captured here so CI's profile + property state can be keyed per block
/// (CI 1.2 §3.2.2 — separate `ChannelProfileStates` per Function Block).
struct FunctionBlockInfo {
    FunctionBlockId id;
    std::string name;          ///< UMP Function Block Name (UTF-8)
    uint8_t first_group = 0;   ///< First UMP group included (0..15)
    uint8_t group_count = 1;   ///< Number of UMP groups in the block
    bool active = true;        ///< Whether the block is currently advertised
    bool is_midi1 = false;     ///< true = MIDI 1 protocol; false = MIDI 2
    bool ui_hint_receiver = true;
    bool ui_hint_sender = true;
};

/// Profile Details Inquiry/Reply payload (CI 1.2 §7.6) — per-profile
/// metadata the responder publishes when asked for "more about profile X".
/// The payload is opaque to the framework — profile authors define its
/// schema. Pulp carries it as raw bytes plus a profile-id + target tuple.
struct ProfileDetails {
    ProfileId id;
    uint8_t target = 0;            ///< Profile-defined target byte
    std::vector<uint8_t> data;     ///< Profile-defined data payload
};

/// MIDI-CI Discovery manager — handles CI message exchange.
///
/// RT-safety contract (audited 2026-05-26): every public method is annotated
/// below. As a rule, CI is a UI/main
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

    // ── Function Block support (CI 1.2 §3.2) ──────────────────────────
    //
    // Function Blocks are MIDI 2.0's logical grouping for UMP endpoints.
    // CI uses them to scope profile + property state per-block so a single
    // endpoint can publish (for example) a piano profile on block 0 and a
    // drum-pad profile on block 1 without the two state machines
    // colliding. Pulp's Function Block surface is responder-side metadata
    // today; the consumer/inquirer side ships as part of UMP endpoint
    // discovery in `pulp::midi::UmpSession`.

    /// NOT RT-safe — `std::vector::push_back` may reallocate. Call from
    /// the main / setup thread before the responder is exposed to peers.
    void add_function_block(const FunctionBlockInfo& block) {
        function_blocks_.push_back(block);
    }
    /// RT-safe. Returns a reference; no allocation.
    const std::vector<FunctionBlockInfo>& function_blocks() const {
        return function_blocks_;
    }
    /// RT-safe. Linear scan over the function-block vector — small N
    /// (≤ 32 by spec).
    const FunctionBlockInfo* find_function_block(FunctionBlockId id) const {
        for (auto& fb : function_blocks_) {
            if (fb.id == id) return &fb;
        }
        return nullptr;
    }

    // ── Profile Inquiry / Add / Remove (CI 1.2 §7) ────────────────────
    //
    // CI 1.2 distinguishes Inquiry (request the full list), Reply
    // (responder publishes the list), Added Report (responder pushes
    // "I now expose this profile"), and Removed Report (responder
    // pushes "this profile is gone"). Pulp owned Inquiry + Reply
    // already (`add_profile` / `profiles()` / `handle_profile_inquiry`);
    // the explicit Add / Remove helpers below complete the wire surface.

    /// NOT RT-safe — allocates the wire bytes. Send this from the
    /// responder when a profile becomes available so connected
    /// inquirers can update their cache without re-polling.
    std::vector<uint8_t> create_profile_added_report(MUID destination,
                                                     const ProfileId& id);

    /// NOT RT-safe — allocates the wire bytes. Mirror of the above for
    /// a profile that's just been removed.
    std::vector<uint8_t> create_profile_removed_report(MUID destination,
                                                       const ProfileId& id);

    // ── Profile Details (CI 1.2 §7.6) ─────────────────────────────────
    //
    // Profile Details Inquiry asks "give me more info about profile X
    // (target byte Y)"; Reply carries an opaque payload defined by the
    // profile spec. Pulp publishes details via `set_profile_details()`
    // and answers inquiries from the local store. Inquirers consume
    // replies via `discovered_profile_details()`.

    /// NOT RT-safe — copies into a std::map; the value's
    /// `std::vector<uint8_t>` is also copied.
    void set_profile_details(const ProfileId& id, uint8_t target,
                             const std::vector<uint8_t>& data);

    /// NOT RT-safe — allocates a result vector and copies stored data.
    std::vector<ProfileDetails> profile_details_for(const ProfileId& id) const;

    /// NOT RT-safe — allocates the wire bytes.
    std::vector<uint8_t> create_profile_details_inquiry(MUID destination,
                                                        const ProfileId& id,
                                                        uint8_t target) const;

    /// RT-safe. Returns a reference; no allocation. The vector mutates
    /// on the dispatch thread; treat as a snapshot.
    const std::vector<ProfileDetails>& discovered_profile_details() const {
        return discovered_details_;
    }

    // ── Profile-Specific Data (CI 1.2 §7.7) ───────────────────────────
    //
    // Profile-Specific Data is the catch-all for profile-defined runtime
    // messaging. The wire envelope is fixed — F0 7E ... 0x2F src dst
    // profile_id(5) length(2) data ... F7 — and the payload semantics
    // are defined by the profile spec, not by CI.

    /// NOT RT-safe — allocates the wire bytes.
    std::vector<uint8_t> create_profile_specific_data(MUID destination,
                                                      const ProfileId& id,
                                                      const uint8_t* data,
                                                      std::size_t size) const;

    inline std::vector<uint8_t> create_profile_specific_data(
        MUID destination, const ProfileId& id,
        const std::vector<uint8_t>& data) const {
        return create_profile_specific_data(destination, id, data.data(), data.size());
    }

    /// Fires when a Profile-Specific Data message arrives. The callback
    /// runs on the same thread that called `process_message()` — NOT the
    /// audio thread.
    std::function<void(MUID source, const ProfileId& id,
                       const std::vector<uint8_t>& data)>
        on_profile_specific_data;

    /// Callbacks
    std::function<void(const CiDeviceInfo&)> on_device_discovered;
    std::function<void(const ProfileId&, bool enabled)> on_profile_changed;
    std::function<void(MUID source, const ProfileId&)> on_profile_added;
    std::function<void(MUID source, const ProfileId&)> on_profile_removed;
    std::function<void(MUID source, const ProfileDetails&)> on_profile_details;

    /// Fires when a Subscribe peer should receive a Notify. Set this
    /// to wire the manager's fan-out to the actual MIDI transport.
    /// Subscribe/Notify dispatcher.
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
    std::vector<FunctionBlockInfo> function_blocks_;
    // Local profile-details store, keyed by (bank, number, version, level, target)
    // to allow multiple target bytes per profile id.
    std::vector<ProfileDetails> local_details_;
    std::vector<ProfileDetails> discovered_details_;
    std::map<std::string, std::string, std::less<>> properties_;
    mutable std::unique_ptr<PeSubscriptionManager> sub_mgr_;

    std::vector<uint8_t> handle_discovery(const uint8_t* data, size_t size);
    std::vector<uint8_t> handle_profile_inquiry(const uint8_t* data, size_t size);
    std::vector<uint8_t> handle_profile_added(const uint8_t* data, size_t size);
    std::vector<uint8_t> handle_profile_removed(const uint8_t* data, size_t size);
    std::vector<uint8_t> handle_profile_details_inquiry(const uint8_t* data, size_t size);
    void handle_profile_details_reply(const uint8_t* data, size_t size);
    void handle_profile_specific_data(const uint8_t* data, size_t size);
    std::vector<uint8_t> handle_subscribe(const uint8_t* data, size_t size);
    void handle_notify(const uint8_t* data, size_t size);
};

}  // namespace pulp::midi
