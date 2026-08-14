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

#include <memory>
#include <mutex>
#include <string>
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

    explicit CoreMidiUmpEndpoint(UmpEndpointInfo info)
        : info_(std::move(info)), callback_state_(std::make_shared<CallbackState>()) {}

    ~CoreMidiUmpEndpoint() override {
        {
            std::lock_guard<std::mutex> lk(callback_state_->mu);
            callback_state_->active = false;
            callback_state_->callback = nullptr;
        }
        if (in_port_) MIDIPortDispose(in_port_);
        if (out_port_) MIDIPortDispose(out_port_);
    }

    const UmpEndpointInfo& info() const noexcept override { return info_; }

    void set_receive_callback(UmpReceiveCallback cb) override {
        std::lock_guard<std::mutex> lk(callback_state_->mu);
        if (callback_state_->active) {
            callback_state_->callback = std::move(cb);
        }
    }

    bool send(const UmpPacket& packet) override {
        if (!info_.direction.can_send || !dest_ || !out_port_) return false;
        if (packet.word_count < 1 || packet.word_count > 4) return false;
        MIDIEventList list;
        MIDIEventPacket* mep = MIDIEventListInit(&list, kMIDIProtocol_2_0);
        mep = MIDIEventListAdd(&list, sizeof(list), mep, 0,
                               static_cast<ByteCount>(packet.word_count),
                               packet.words.data());
        if (!mep) return false;
        OSStatus st = MIDISendEventList(out_port_, dest_, &list);
        return st == noErr;
    }

    bool is_open() const noexcept override { return open_; }

    std::shared_ptr<CallbackState> callback_state() const {
        return callback_state_;
    }

    // Setters used during construction by os_open. They're not part of
    // the public surface — only the backend reaches in to wire up
    // CoreMIDI handles after construction.
    void set_ports(MIDIPortRef in_port, MIDIEndpointRef src,
                   MIDIPortRef out_port, MIDIEndpointRef dest) {
        in_port_ = in_port;
        src_ = src;
        out_port_ = out_port;
        dest_ = dest;
        open_ = true;
    }

private:
    UmpEndpointInfo info_;
    MIDIPortRef in_port_ = 0;
    MIDIEndpointRef src_ = 0;
    MIDIPortRef out_port_ = 0;
    MIDIEndpointRef dest_ = 0;
    bool open_ = false;
    std::shared_ptr<CallbackState> callback_state_;
};

struct OsState {
    MIDIClientRef client = 0;
    std::mutex mu;
    std::unordered_map<std::string, std::unique_ptr<CoreMidiUmpEndpoint>> endpoints;
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

std::string topology_id(MIDIEndpointRef endpoint) {
    MIDIEntityRef entity = 0;
    if (MIDIEndpointGetEntity(endpoint, &entity) == noErr && entity) {
        if (auto id = object_unique_id(entity); !id.empty()) return id;
        MIDIDeviceRef device = 0;
        if (MIDIEntityGetDevice(entity, &device) == noErr && device) {
            if (auto id = object_unique_id(device); !id.empty()) return id;
        }
    }
    // Virtual endpoints commonly have no entity. They remain independent
    // one-direction groups, identified by their own unique ID.
    return object_unique_id(endpoint);
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
    std::unordered_map<std::string, EndpointGroup> groups;
    const auto add = [&](MIDIEndpointRef endpoint, bool is_source) {
        const auto id = topology_id(endpoint);
        if (id.empty()) return;
        auto info = info_for_endpoint(endpoint, is_source, id);
        auto [it, inserted] = groups.try_emplace(id);
        auto& group = it->second;
        if (inserted) group.info = std::move(info);
        if (is_source) {
            group.source = endpoint;
            group.info.direction.can_receive = true;
        } else {
            group.destination = endpoint;
            group.info.direction.can_send = true;
        }
        if (group.info.name.empty()) group.info.name = std::move(info.name);
    };

    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i) {
        add(MIDIGetSource(i), true);
    }
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i) {
        add(MIDIGetDestination(i), false);
    }
    return groups;
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

bool os_init(const UmpSessionConfig& cfg, void** out_state) {
    if (!coremidi_ump_available()) return false;

    auto state = std::make_unique<OsState>();
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault,
                                                 cfg.name.c_str(),
                                                 kCFStringEncodingUTF8);
    OSStatus st = MIDIClientCreate(name ? name : CFSTR("Pulp UMP Session"),
                                   nullptr, nullptr, &state->client);
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
    auto existing = state->endpoints.find(id);
    if (existing != state->endpoints.end()) {
        if (status) *status = UmpOpenStatus::Ok;
        return existing->second.get();
    }

    auto groups = endpoint_groups();
    auto group_it = groups.find(id);
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
    auto ep = std::make_unique<CoreMidiUmpEndpoint>(std::move(info));
    auto callback_state = ep->callback_state();

    MIDIPortRef in_port = 0;
    MIDIPortRef out_port = 0;
    if (src) {
        OSStatus st = MIDIInputPortCreateWithProtocol(
            state->client, CFSTR("Pulp UMP In"), kMIDIProtocol_2_0, &in_port,
            ^(const MIDIEventList* evtlist, void* /*src_conn*/) {
                static constexpr uint8_t kWordsByType[16] = {
                    1, 1, 1, 2, 2, 4, 4, 1,
                    2, 2, 2, 3, 3, 4, 4, 4
                };
                const MIDIEventPacket* packet = &evtlist->packet[0];
                for (UInt32 i = 0; i < evtlist->numPackets; ++i) {
                    UInt32 idx = 0;
                    while (idx < packet->wordCount) {
                        uint32_t w0 = packet->words[idx];
                        uint8_t type = (w0 >> 28) & 0x0F;
                        uint8_t words = kWordsByType[type];
                        if (idx + words > packet->wordCount) break;
                        UmpPacket p;
                        p.word_count = static_cast<int>(words);
                        for (uint8_t k = 0; k < words; ++k) {
                            p.words[k] = packet->words[idx + k];
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
                        idx += words;
                    }
                    packet = MIDIEventPacketNext(packet);
                }
            });
        if (st != noErr || !in_port) {
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
