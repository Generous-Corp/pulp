// CoreMIDI 2.0 backend for UmpSession on macOS and iOS.
//
// Wires the Pulp UmpSession to a real `MIDIClientRef`. Discovery uses
// `MIDIGetNumberOfSources()` / `MIDIGetNumberOfDestinations()`; per-
// endpoint connections use `MIDIInputPortCreateWithProtocol` with
// `kMIDIProtocol_2_0` and `MIDISendEventList()` on the output side.
//
// The OS-backed `UmpEndpoint` lives here too: `CoreMidiUmpEndpoint`
// wraps the source/destination endpoints belonging to one CoreMIDI entity.
// Endpoint unique IDs are globally unique and therefore are not a pairing
// key; entity (or, when needed, device) topology is.
//
// Lifetime:
//   - The session's CoreMIDI client (`MIDIClientRef`) is owned by
//     `OsState` and disposed in `os_shutdown`.
//   - Endpoint objects are owned by `OsState::endpoints`. Input blocks retain
//     only a shared callback state that is deactivated before port disposal,
//     so callback teardown never dereferences a destroyed endpoint.
//
// The event-list API is available on macOS 11+ and iOS 14+. Older Apple
// runtimes keep the cross-platform virtual-endpoint-only fallback.

#import <CoreMIDI/CoreMIDI.h>
#import <TargetConditionals.h>

#include <mach/mach_time.h>

#include <pulp/midi/ump_endpoint.hpp>
#include <pulp/midi/ump_session.hpp>
#include <pulp/runtime/log.hpp>

#include "../../src/ump_session_backend.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pulp::midi {

namespace {

bool coremidi_ump_available() {
#if TARGET_OS_IPHONE
    if (@available(iOS 14.0, *)) return true;
#else
    if (@available(macOS 11.0, *)) return true;
#endif
    return false;
}

/// Shared topology record owned by the notification block.
///
/// The block deliberately touches only memory it owns (a shared_ptr), so a
/// notification delivered concurrently with MIDIClientDispose cannot reach a
/// destroyed OsState. That property is preserved here.
///
/// Three things this is NOT, each because the simpler version is wrong:
///
///  * Not a bare counter. A single global generation made ANY removal anywhere
///    permanently invalidate EVERY open endpoint in the process -- unplugging
///    an unrelated controller silently killed every live session.
///  * Not an unbounded, un-scoped list of removed refs. `MIDIObjectRef` is a
///    reusable UInt32 handle with no cross-replug uniqueness guarantee, so a
///    ref recorded BEFORE an endpoint opened can later match that endpoint's
///    reused handle and close a healthy endpoint. Entries therefore carry the
///    generation at which they were recorded, and a reader ignores anything at
///    or before its own open.
///  * Not mutex-protected on the read path. `is_open()` is reached from
///    `send()`, which the public header documents as non-blocking and lock-free
///    where the OS allows; taking a mutex there would block an audio thread
///    behind a notify thread that allocates under the same lock. Readers are
///    lock-free; only the writer serialises.
struct TopologyRecord {
    static constexpr std::size_t kMaxRemoved = 256;

    struct Entry {
        MIDIObjectRef ref = 0;
        std::uint64_t seq = 0;
    };

    std::atomic<std::uint64_t> generation{0};
    /// Published count. An entry is fully written before `count` exposes it, so
    /// a reader that acquires `count` sees complete entries below it.
    std::atomic<std::size_t> count{0};
    Entry entries[kMaxRemoved]{};
    /// Generation through which history has been dropped. Only endpoints opened
    /// at or before this can no longer be proven unaffected.
    std::atomic<std::uint64_t> evicted_through_seq{0};
    /// Serialises writers only. CoreMIDI delivers notifications on one runloop,
    /// but relying on that for memory safety would be an undocumented bet.
    std::mutex writer_mu;

    void note_removed(MIDIObjectRef object) {
        std::lock_guard<std::mutex> lk(writer_mu);
        const auto seq = generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        const auto n = count.load(std::memory_order_relaxed);
        if (n < kMaxRemoved) {
            entries[n].ref = object;
            entries[n].seq = seq;
            count.store(n + 1, std::memory_order_release);
        } else {
            // Full. Record that history up to here is incomplete rather than
            // silently forgetting a removal.
            evicted_through_seq.store(seq, std::memory_order_release);
        }
    }

    /// True if any handle this endpoint depends on was removed after it opened.
    ///
    /// Matches the entity and device as well as the two endpoint refs: CoreMIDI
    /// may report only the parent object on a hot-unplug, and matching solely on
    /// source/destination would absorb that and leave the endpoint reporting
    /// open while holding dangling refs -- failing OPEN, which is worse than the
    /// global latch this replaced.
    bool was_removed(MIDIEndpointRef source, MIDIEndpointRef destination,
                     MIDIObjectRef entity, MIDIObjectRef device,
                     std::uint64_t open_seq) const noexcept {
        // Fail closed only for endpoints old enough to be affected by the gap.
        if (evicted_through_seq.load(std::memory_order_acquire) > open_seq) return true;
        const auto n = count.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < n; ++i) {
            if (entries[i].seq <= open_seq) continue;  // pre-open record: inert
            const auto ref = entries[i].ref;
            if ((source != 0 && ref == source) || (destination != 0 && ref == destination) ||
                (entity != 0 && ref == entity) || (device != 0 && ref == device)) {
                return true;
            }
        }
        return false;
    }
};

class CoreMidiUmpEndpoint : public UmpEndpoint {
public:
    // Receive-side teardown gate.
    //
    // The sender side has always been fenced: deactivate() sets a closing bit
    // and drains in-flight sends before disposing the port. The receive side
    // was not, so a callback that had already been snapshotted could still be
    // executing after deactivate() returned and the endpoint was destroyed.
    // That pushed an internal teardown race onto every consumer -- callers
    // would have had to make every capture independently lifetime-safe to use
    // the API correctly, which the header did not say and which no caller can
    // reliably reason about.
    //
    // The contract is now symmetric and explicit:
    //   * retirement stops NEW receive callbacks from being admitted;
    //   * already-admitted callbacks are drained before endpoint destruction
    //     returns;
    //   * a callback invocation therefore happens-before deactivate() returns.
    struct CallbackState {
        std::mutex mu;
        bool active = true;
        UmpReceiveCallback callback;

        static constexpr uint32_t kClosingBit = uint32_t{1} << 31;
        static constexpr uint32_t kReceiverCountMask = kClosingBit - 1;
        std::atomic<uint32_t> receiver_state{0};

        /// Admits one in-flight receive callback, or refuses once closing.
        bool acquire_receiver() noexcept {
            uint32_t state = receiver_state.load(std::memory_order_acquire);
            while ((state & kClosingBit) == 0) {
                if ((state & kReceiverCountMask) == kReceiverCountMask) return false;
                if (receiver_state.compare_exchange_weak(state, state + 1,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
                    return true;
                }
            }
            return false;
        }

        void release_receiver() noexcept {
            receiver_state.fetch_sub(1, std::memory_order_acq_rel);
        }

        void begin_closing_receivers() noexcept {
            uint32_t state = receiver_state.load(std::memory_order_acquire);
            while ((state & kClosingBit) == 0 &&
                   !receiver_state.compare_exchange_weak(state, state | kClosingBit,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {}
        }

        /// Returns only once every admitted callback has finished executing.
        void drain_receivers() noexcept {
            while ((receiver_state.load(std::memory_order_acquire) & kReceiverCountMask) != 0) {
                std::this_thread::yield();
            }
        }
    };

    /// Scope guard so an early return inside the read block cannot leak a count.
    struct ReceiverGuard {
        CallbackState* state;
        ~ReceiverGuard() { state->release_receiver(); }
    };

    CoreMidiUmpEndpoint(UmpEndpointInfo info,
                        std::shared_ptr<TopologyRecord> generation)
        : info_(std::move(info)),
          callback_state_(std::make_shared<CallbackState>()),
          generation_(std::move(generation)),
          generation_at_open_(generation_->generation.load(std::memory_order_acquire)) {}

    ~CoreMidiUmpEndpoint() override {
        deactivate();
    }

    /// Non-blocking half of teardown. Refuses new sends and new receive
    /// callbacks and drops the user callback, but does NOT wait for in-flight
    /// work. Safe to call while holding OsState::mu.
    void retire() noexcept {
        callback_state_->begin_closing_receivers();
        {
            std::lock_guard<std::mutex> lk(callback_state_->mu);
            callback_state_->active = false;
            callback_state_->callback = nullptr;
        }
        begin_closing();
        open_.store(false, std::memory_order_release);
    }

    /// Blocking half. Waits for in-flight sends and receive callbacks, then
    /// disposes the ports.
    ///
    /// MUST NOT be called while holding OsState::mu. The receive drain waits on
    /// arbitrary user callback code, and a callback that calls back into
    /// UmpSession (a reasonable reaction to a hot-plug) would then deadlock
    /// against the very lock that gates open_endpoint(). The sender-only drain
    /// never had this hazard because send() runs no user code; making the
    /// receive side symmetric introduced it, so the lock scope has to shrink.
    void deactivate() noexcept {
        retire();
        // Neither drain is performed under callback_state_->mu -- the read block
        // takes that mutex to snapshot the callback, so draining under it would
        // deadlock directly.
        callback_state_->drain_receivers();
        while ((sender_state_.load(std::memory_order_acquire) &
                kSenderCountMask) != 0) {
            std::this_thread::yield();
        }
        const auto in_port = in_port_.exchange(0, std::memory_order_acq_rel);
        const auto src = src_.exchange(0, std::memory_order_acq_rel);
        const auto out_port = out_port_.exchange(0, std::memory_order_acq_rel);
        dest_.store(0, std::memory_order_release);
        if (in_port) {
            if (src) MIDIPortDisconnectSource(in_port, src);
            MIDIPortDispose(in_port);
        }
        if (out_port) MIDIPortDispose(out_port);
    }

    const UmpEndpointInfo& info() const noexcept override { return info_; }

    void set_receive_callback(UmpReceiveCallback cb) override {
        std::lock_guard<std::mutex> lk(callback_state_->mu);
        if (callback_state_->active) {
            callback_state_->callback = std::move(cb);
        }
    }

    bool send(const UmpPacket& packet) noexcept override;

    bool is_open() const noexcept override;

    void set_parent_refs(MIDIObjectRef entity, MIDIObjectRef device) noexcept {
        entity_ref_.store(entity, std::memory_order_release);
        device_ref_.store(device, std::memory_order_release);
    }

    std::shared_ptr<CallbackState> callback_state() const {
        return callback_state_;
    }

    // Setters used during construction by os_open. They're not part of
    // the public surface — only the backend reaches in to wire up
    // CoreMIDI handles after construction.
    void set_ports(MIDIPortRef in_port, MIDIEndpointRef src,
                   MIDIPortRef out_port, MIDIEndpointRef dest) {
        in_port_.store(in_port, std::memory_order_relaxed);
        src_.store(src, std::memory_order_relaxed);
        out_port_.store(out_port, std::memory_order_relaxed);
        dest_.store(dest, std::memory_order_relaxed);
        sender_state_.store(0, std::memory_order_release);
        open_.store(true, std::memory_order_release);
    }

    bool matches(MIDIEndpointRef src, MIDIEndpointRef dest) const noexcept {
        return src_.load(std::memory_order_acquire) == src &&
               dest_.load(std::memory_order_acquire) == dest;
    }

private:
    static constexpr uint32_t kClosingBit = uint32_t{1} << 31;
    static constexpr uint32_t kSenderCountMask = kClosingBit - 1;

    bool acquire_sender() noexcept {
        uint32_t state = sender_state_.load(std::memory_order_acquire);
        while ((state & kClosingBit) == 0) {
            if ((state & kSenderCountMask) == kSenderCountMask) return false;
            if (sender_state_.compare_exchange_weak(
                    state, state + 1, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void begin_closing() const noexcept {
        uint32_t state = sender_state_.load(std::memory_order_acquire);
        while ((state & kClosingBit) == 0 &&
               !sender_state_.compare_exchange_weak(
                   state, state | kClosingBit, std::memory_order_acq_rel,
                   std::memory_order_acquire)) {}
    }

    UmpEndpointInfo info_;
    static_assert(std::atomic<MIDIPortRef>::is_always_lock_free);
    static_assert(std::atomic<MIDIEndpointRef>::is_always_lock_free);
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    std::atomic<MIDIPortRef> in_port_{0};
    std::atomic<MIDIEndpointRef> src_{0};
    std::atomic<MIDIPortRef> out_port_{0};
    std::atomic<MIDIEndpointRef> dest_{0};
    mutable std::atomic<bool> open_{false};
    // One CAS domain closes admission and counts in-flight senders, so
    // teardown cannot observe zero between a separate gate check/increment.
    mutable std::atomic<uint32_t> sender_state_{0};
    std::shared_ptr<CallbackState> callback_state_;
    std::shared_ptr<TopologyRecord> generation_;
    mutable std::atomic<uint64_t> generation_at_open_{0};
    // Parent handles, so a device-level removal notification is not absorbed.
    std::atomic<MIDIObjectRef> entity_ref_{0};
    std::atomic<MIDIObjectRef> device_ref_{0};
};

struct OsState {
    MIDIClientRef client = 0;
    std::mutex mu;
    std::unordered_map<std::string, std::unique_ptr<CoreMidiUmpEndpoint>> endpoints;
    // Borrowed UmpEndpoint pointers remain valid until session teardown, even
    // after topology invalidates an open and a same-ID wrapper replaces it.
    std::vector<std::unique_ptr<CoreMidiUmpEndpoint>> retired;
    std::shared_ptr<TopologyRecord> topology_generation =
        std::make_shared<TopologyRecord>();
};

std::string object_unique_id(MIDIObjectRef object) {
    SInt32 unique_id = 0;
    if (!object ||
        MIDIObjectGetIntegerProperty(object, kMIDIPropertyUniqueID,
                                     &unique_id) != noErr ||
        unique_id == 0) {
        return {};
    }
    return std::to_string(unique_id);
}

std::string entity_topology_id(MIDIEndpointRef endpoint) {
    MIDIEntityRef entity = 0;
    if (MIDIEndpointGetEntity(endpoint, &entity) == noErr && entity) {
        if (auto id = object_unique_id(entity); !id.empty()) return id;
        MIDIDeviceRef device = 0;
        if (MIDIEntityGetDevice(entity, &device) == noErr && device) {
            if (auto id = object_unique_id(device); !id.empty()) return id;
        }
    }
    return {};
}

/// Live parent handles for an endpoint. The topology ledger records removals of
/// Device and Entity objects as well as Source/Destination, and CoreMIDI may
/// report only the parent on a hot-unplug, so an endpoint has to know its
/// parents to recognise its own removal.
void parent_refs_for_endpoint(MIDIEndpointRef endpoint, MIDIObjectRef& entity_out,
                              MIDIObjectRef& device_out) {
    entity_out = 0;
    device_out = 0;
    MIDIEntityRef entity = 0;
    if (MIDIEndpointGetEntity(endpoint, &entity) == noErr && entity) {
        entity_out = entity;
        MIDIDeviceRef device = 0;
        if (MIDIEntityGetDevice(entity, &device) == noErr && device) device_out = device;
    }
}

UmpEndpointInfo info_for_endpoint(MIDIEndpointRef ep, bool is_source,
                                  std::string id) {
    UmpEndpointInfo info;
    info.id = std::move(id);

    CFStringRef name = nullptr;
    MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name);
    if (name) {
        char buf[256]{};
        if (CFStringGetCString(name, buf, sizeof(buf),
                              kCFStringEncodingUTF8)) {
            info.name = buf;
        }
        CFRelease(name);
    }
    info.direction.can_receive = is_source;
    info.direction.can_send = !is_source;
    return info;
}

struct EndpointGroup {
    UmpEndpointInfo info;
    MIDIEndpointRef source = 0;
    MIDIEndpointRef destination = 0;
};

std::unordered_map<std::string, EndpointGroup> endpoint_groups() {
    struct RawEndpoint {
        CoreMidiUmpTopologyEndpoint topology;
        MIDIEndpointRef ref = 0;
    };
    std::vector<RawEndpoint> raw;
    const auto add = [&](MIDIEndpointRef endpoint, bool is_source) {
        const auto endpoint_id = object_unique_id(endpoint);
        if (endpoint_id.empty()) return;
        auto info = info_for_endpoint(endpoint, is_source, endpoint_id);
        raw.push_back({{endpoint_id, entity_topology_id(endpoint),
                        std::move(info.name), is_source},
                       endpoint});
    };

    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i) {
        add(MIDIGetSource(i), true);
    }
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i) {
        add(MIDIGetDestination(i), false);
    }

    std::vector<CoreMidiUmpTopologyEndpoint> topology;
    topology.reserve(raw.size());
    for (const auto& endpoint : raw) topology.push_back(endpoint.topology);

    std::unordered_map<std::string, EndpointGroup> groups;
    for (auto& info : group_coremidi_ump_topology(topology)) {
        EndpointGroup group;
        group.info = std::move(info);
        if (group.info.id.rfind("entity:", 0) == 0) {
            const auto entity_id = group.info.id.substr(7);
            for (const auto& endpoint : raw) {
                if (endpoint.topology.entity_id != entity_id) continue;
                if (endpoint.topology.is_source) group.source = endpoint.ref;
                else group.destination = endpoint.ref;
            }
        } else {
            for (const auto& endpoint : raw) {
                if (endpoint.topology.endpoint_id != group.info.id) continue;
                if (endpoint.topology.is_source) group.source = endpoint.ref;
                else group.destination = endpoint.ref;
                break;
            }
        }
        groups.emplace(group.info.id, std::move(group));
    }
    return groups;
}

bool CoreMidiUmpEndpoint::is_open() const noexcept {
    if ((sender_state_.load(std::memory_order_acquire) & kClosingBit) != 0 ||
        !open_.load(std::memory_order_acquire)) return false;
    const auto generation = generation_->generation.load(std::memory_order_acquire);
    if (generation != generation_at_open_.load(std::memory_order_acquire)) {
        // Something was removed. Close only if it was OURS; an unrelated device
        // being unplugged must not take this endpoint down with it.
        if (generation_->was_removed(src_.load(std::memory_order_acquire),
                                     dest_.load(std::memory_order_acquire),
                                     entity_ref_.load(std::memory_order_acquire),
                                     device_ref_.load(std::memory_order_acquire),
                                     generation_at_open_.load(std::memory_order_acquire))) {
            begin_closing();
            open_.store(false, std::memory_order_release);
            return false;
        }
        // Unrelated change: absorb it so we do not re-scan on every call.
        generation_at_open_.store(generation, std::memory_order_release);
    }
    return true;
}

bool CoreMidiUmpEndpoint::send(const UmpPacket& packet) noexcept {
    if (packet.word_count < 1 || packet.word_count > 4) return false;
    if (!info_.direction.can_send || !acquire_sender()) return false;
    struct SenderGuard {
        std::atomic<uint32_t>& state;
        ~SenderGuard() { state.fetch_sub(1, std::memory_order_acq_rel); }
    } guard{sender_state_};
    if (!is_open()) return false;
    const auto out_port = out_port_.load(std::memory_order_acquire);
    const auto dest = dest_.load(std::memory_order_acquire);
    if (!out_port || !dest) return false;
    MIDIEventList list;
    MIDIEventPacket* mep = MIDIEventListInit(&list, kMIDIProtocol_2_0);
    mep = MIDIEventListAdd(&list, sizeof(list), mep, 0,
                           static_cast<ByteCount>(packet.word_count),
                           packet.words.data());
    if (!mep) return false;
    const OSStatus status = MIDISendEventList(out_port, dest, &list);
    if (status != noErr) open_.store(false, std::memory_order_release);
    return status == noErr;
}

double host_ticks_to_seconds(MIDITimeStamp ticks) {
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    if (timebase.denom == 0) return 0.0;
    return static_cast<double>(static_cast<long double>(ticks) *
                               static_cast<long double>(timebase.numer) /
                               static_cast<long double>(timebase.denom) /
                               1.0e9L);
}

bool is_topology_object_type(MIDIObjectType type) {
    const auto base = static_cast<MIDIObjectType>(
        type & ~kMIDIObjectType_ExternalMask);
    return base == kMIDIObjectType_Device ||
           base == kMIDIObjectType_Entity ||
           base == kMIDIObjectType_Source ||
           base == kMIDIObjectType_Destination;
}

bool notification_affects_topology(const MIDINotification* notification) {
    if (!notification) return false;
    // Additions do not invalidate handles that are already open. Only removal
    // can make a cached CoreMIDI object unusable; enumerate/open perform a
    // fresh census when callers want newly added endpoints.
    if (notification->messageID == kMIDIMsgObjectRemoved) {
        const auto* change =
            reinterpret_cast<const MIDIObjectAddRemoveNotification*>(
                notification);
        return is_topology_object_type(change->childType);
    }
    return false;
}

bool os_init(const UmpSessionConfig& cfg, void** out_state) {
    if (!coremidi_ump_available()) return false;

    auto state = std::make_unique<OsState>();
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault,
                                                 cfg.name.c_str(),
                                                 kCFStringEncodingUTF8);
    auto generation = state->topology_generation;
    OSStatus st = MIDIClientCreateWithBlock(
        name ? name : CFSTR("Pulp UMP Session"), &state->client,
        ^(const MIDINotification* notification) {
            if (notification_affects_topology(notification)) {
                const auto* change =
                    reinterpret_cast<const MIDIObjectAddRemoveNotification*>(
                        notification);
                generation->note_removed(change->child);
            }
        });
    if (name) CFRelease(name);
    if (st != noErr) {
        runtime::log_warn("CoreMIDI: UmpSession client create failed ({})",
                          static_cast<int>(st));
        return false;
    }
    *out_state = state.release();
    return true;
}

void os_shutdown(void* opaque) {
    auto* state = static_cast<OsState*>(opaque);
    if (!state) return;
    // Dispose endpoint ports before the client (CoreMIDI requirement).
    //
    // Move the containers out under the lock, then let them destruct AFTER
    // releasing it. ~CoreMidiUmpEndpoint -> deactivate() blocks on the receive
    // drain, which waits on arbitrary user callback code; destroying them under
    // state->mu would let a callback that touches UmpSession deadlock shutdown.
    decltype(state->endpoints) endpoints;
    decltype(state->retired) retired;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        endpoints.swap(state->endpoints);
        retired.swap(state->retired);
    }
    endpoints.clear();
    retired.clear();
    if (state->client) {
        MIDIClientDispose(state->client);
        state->client = 0;
    }
    delete state;
}

std::vector<UmpEndpointInfo> os_enumerate(void* opaque) {
    std::vector<UmpEndpointInfo> out;
    auto* state = static_cast<OsState*>(opaque);
    if (!state) return out;

    auto groups = endpoint_groups();
    out.reserve(groups.size());
    for (auto& [_, group] : groups) out.push_back(std::move(group.info));
    return out;
}

UmpEndpoint* os_open(void* opaque, const std::string& id, UmpOpenStatus* status) {
    auto* state = static_cast<OsState*>(opaque);
    if (!state || !state->client) {
        if (status) *status = UmpOpenStatus::OsBackendUnavailable;
        return nullptr;
    }
    // Serialize the lookup-and-create transaction. A check before port
    // creation followed by an unlocked insertion lets concurrent callers
    // replace the first unique_ptr and invalidate its borrowed pointer.
    std::lock_guard<std::mutex> state_lock(state->mu);
    auto groups = endpoint_groups();
    auto group_it = groups.find(id);
    auto existing = state->endpoints.find(id);
    if (existing != state->endpoints.end()) {
        if (group_it != groups.end() && existing->second->is_open() &&
            existing->second->matches(group_it->second.source,
                                      group_it->second.destination)) {
            if (status) *status = UmpOpenStatus::Ok;
            return existing->second.get();
        }
        // retire(), not deactivate(): the blocking receive drain must not run
        // under state->mu. A user receive callback that calls back into
        // UmpSession would otherwise deadlock against this very lock. The
        // retiree is unreachable through the map from here on, and its drain and
        // port disposal happen in os_shutdown, outside the lock.
        existing->second->retire();
        state->retired.push_back(std::move(existing->second));
        state->endpoints.erase(existing);
    }

    if (group_it == groups.end()) {
        if (status) *status = UmpOpenStatus::NotFound;
        return nullptr;
    }
    MIDIEndpointRef src = group_it->second.source;
    MIDIEndpointRef dest = group_it->second.destination;
    if (!src && !dest) {
        if (status) *status = UmpOpenStatus::NotFound;
        return nullptr;
    }

    UmpEndpointInfo info = group_it->second.info;

    // Construct the endpoint up front so the block can retain its fenced
    // callback state. The session owns the endpoint itself.
    auto ep = std::make_unique<CoreMidiUmpEndpoint>(
        std::move(info), state->topology_generation);
    {
        // Record the parents of whichever handle we have, so a device-level
        // removal notification is recognised instead of absorbed.
        MIDIObjectRef entity = 0;
        MIDIObjectRef device = 0;
        parent_refs_for_endpoint(src ? src : dest, entity, device);
        ep->set_parent_refs(entity, device);
    }
    auto callback_state = ep->callback_state();

    MIDIPortRef in_port = 0;
    MIDIPortRef out_port = 0;
    if (src) {
        OSStatus st = MIDIInputPortCreateWithProtocol(
            state->client, CFSTR("Pulp UMP In"), kMIDIProtocol_2_0, &in_port,
            ^(const MIDIEventList* evtlist, void* /*src_conn*/) {
                const MIDIEventPacket* packet = &evtlist->packet[0];
                for (UInt32 i = 0; i < evtlist->numPackets; ++i) {
                    walk_ump_packet(
                        packet->words, packet->wordCount,
                        [&](uint8_t, const uint32_t* words,
                            uint32_t word_count) {
                        UmpPacket p;
                        p.word_count = static_cast<int>(word_count);
                        for (uint32_t k = 0; k < word_count; ++k) {
                            p.words[k] = words[k];
                        }
                        if (!callback_state->acquire_receiver()) {
                            // Retired: refuse admission rather than race teardown.
                            return;
                        }
                        CoreMidiUmpEndpoint::ReceiverGuard guard{callback_state.get()};
                        UmpReceiveCallback callback;
                        {
                            std::lock_guard<std::mutex> lk(callback_state->mu);
                            if (callback_state->active) {
                                callback = callback_state->callback;
                            }
                        }
                        if (callback) {
                            callback(p, host_ticks_to_seconds(packet->timeStamp));
                        }
                    });
                    packet = MIDIEventPacketNext(packet);
                }
            });
        if (st != noErr || !in_port) {
            if (in_port) MIDIPortDispose(in_port);
            if (status) *status = UmpOpenStatus::OsError;
            return nullptr;
        }
        st = MIDIPortConnectSource(in_port, src, nullptr);
        if (st != noErr) {
            MIDIPortDispose(in_port);
            if (status) *status = UmpOpenStatus::OsError;
            return nullptr;
        }
    }
    if (dest) {
        const OSStatus st =
            MIDIOutputPortCreate(state->client, CFSTR("Pulp UMP Out"),
                                 &out_port);
        if (st != noErr || !out_port) {
            if (in_port) MIDIPortDispose(in_port);
            if (out_port) MIDIPortDispose(out_port);
            if (status) *status = UmpOpenStatus::OsError;
            return nullptr;
        }
    }
    ep->set_ports(in_port, src, out_port, dest);

    UmpEndpoint* out_ptr = ep.get();
    state->endpoints.emplace(id, std::move(ep));
    if (status) *status = UmpOpenStatus::Ok;
    return out_ptr;
}

} // namespace

void register_coremidi_ump_backend() {
    static std::once_flag once;
    std::call_once(once, [] {
        ump_os::OsBackendVTable v;
        v.init = &os_init;
        v.shutdown = &os_shutdown;
        v.enumerate = &os_enumerate;
        v.open = &os_open;
        register_ump_os_backend(v);
    });
}

} // namespace pulp::midi
