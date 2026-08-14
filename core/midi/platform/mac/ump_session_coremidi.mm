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

class CoreMidiUmpEndpoint : public UmpEndpoint {
public:
    struct CallbackState {
        std::mutex mu;
        bool active = true;
        UmpReceiveCallback callback;
    };

    CoreMidiUmpEndpoint(UmpEndpointInfo info,
                        std::shared_ptr<std::atomic<uint64_t>> generation)
        : info_(std::move(info)),
          callback_state_(std::make_shared<CallbackState>()),
          generation_(std::move(generation)),
          generation_at_open_(generation_->load(std::memory_order_acquire)) {}

    ~CoreMidiUmpEndpoint() override {
        deactivate();
    }

    void deactivate() noexcept {
        {
            std::lock_guard<std::mutex> lk(callback_state_->mu);
            callback_state_->active = false;
            callback_state_->callback = nullptr;
        }
        closing_.store(true, std::memory_order_release);
        open_.store(false, std::memory_order_release);
        while (active_senders_.load(std::memory_order_acquire) != 0) {
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
        closing_.store(false, std::memory_order_release);
        open_.store(true, std::memory_order_release);
    }

    bool matches(MIDIEndpointRef src, MIDIEndpointRef dest) const noexcept {
        return src_.load(std::memory_order_acquire) == src &&
               dest_.load(std::memory_order_acquire) == dest;
    }

private:
    UmpEndpointInfo info_;
    static_assert(std::atomic<MIDIPortRef>::is_always_lock_free);
    static_assert(std::atomic<MIDIEndpointRef>::is_always_lock_free);
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    std::atomic<MIDIPortRef> in_port_{0};
    std::atomic<MIDIEndpointRef> src_{0};
    std::atomic<MIDIPortRef> out_port_{0};
    std::atomic<MIDIEndpointRef> dest_{0};
    mutable std::atomic<bool> open_{false};
    mutable std::atomic<bool> closing_{false};
    std::atomic<uint32_t> active_senders_{0};
    std::shared_ptr<CallbackState> callback_state_;
    std::shared_ptr<std::atomic<uint64_t>> generation_;
    uint64_t generation_at_open_ = 0;
};

struct OsState {
    MIDIClientRef client = 0;
    std::mutex mu;
    std::unordered_map<std::string, std::unique_ptr<CoreMidiUmpEndpoint>> endpoints;
    // Borrowed UmpEndpoint pointers remain valid until session teardown, even
    // after topology invalidates an open and a same-ID wrapper replaces it.
    std::vector<std::unique_ptr<CoreMidiUmpEndpoint>> retired;
    std::shared_ptr<std::atomic<uint64_t>> topology_generation =
        std::make_shared<std::atomic<uint64_t>>(0);
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
    if (closing_.load(std::memory_order_acquire) ||
        !open_.load(std::memory_order_acquire)) return false;
    if (generation_->load(std::memory_order_acquire) != generation_at_open_) {
        closing_.store(true, std::memory_order_release);
        open_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

bool CoreMidiUmpEndpoint::send(const UmpPacket& packet) noexcept {
    if (packet.word_count < 1 || packet.word_count > 4) return false;
    if (!info_.direction.can_send ||
        closing_.load(std::memory_order_acquire)) {
        return false;
    }
    active_senders_.fetch_add(1, std::memory_order_acq_rel);
    struct SenderGuard {
        std::atomic<uint32_t>& count;
        ~SenderGuard() { count.fetch_sub(1, std::memory_order_acq_rel); }
    } guard{active_senders_};
    if (closing_.load(std::memory_order_acquire) || !is_open()) return false;
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
                generation->fetch_add(1, std::memory_order_acq_rel);
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
    {
        std::lock_guard<std::mutex> lk(state->mu);
        state->endpoints.clear();
        state->retired.clear();
    }
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
        existing->second->deactivate();
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
