// CoreMIDI 2.0 backend for UmpSession (macOS plan item 8.1).
//
// Wires the Pulp UmpSession to a real `MIDIClientRef`. Discovery uses
// `MIDIGetNumberOfSources()` / `MIDIGetNumberOfDestinations()`; per-
// endpoint connections use `MIDIInputPortCreateWithProtocol` with
// `kMIDIProtocol_2_0` and `MIDISendEventList()` on the output side.
//
// The OS-backed `UmpEndpoint` lives here too: `CoreMidiUmpEndpoint`
// wraps a paired (source, destination) for a given unique-id, so a
// caller that asks the session to open id `1234` gets a single
// endpoint that can both send and receive against the physical device.
//
// Lifetime:
//   - The session's CoreMIDI client (`MIDIClientRef`) is owned by
//     `OsState` and disposed in `os_shutdown`.
//   - Endpoint objects are owned by `OsState::endpoints` and the input
//     callback block captures the raw pointer; lifetime is bounded by
//     the session, so the captured pointer stays valid for the block's
//     entire active window.
//
// This file is compiled only when the platform is macOS (non-iOS); the
// cross-platform `ump_session.cpp` falls back to virtual-endpoints-only
// when this TU isn't linked.

#import <CoreMIDI/CoreMIDI.h>

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

class CoreMidiUmpEndpoint : public UmpEndpoint {
public:
    explicit CoreMidiUmpEndpoint(UmpEndpointInfo info)
        : info_(std::move(info)) {}

    ~CoreMidiUmpEndpoint() override {
        if (in_port_) MIDIPortDispose(in_port_);
        if (out_port_) MIDIPortDispose(out_port_);
    }

    const UmpEndpointInfo& info() const noexcept override { return info_; }

    void set_receive_callback(UmpReceiveCallback cb) override {
        std::lock_guard<std::mutex> lk(cb_mu_);
        cb_ = std::move(cb);
    }

    bool send(const UmpPacket& packet) override {
        if (!info_.direction.can_send || !dest_ || !out_port_) return false;
        if (packet.word_count < 1 || packet.word_count > 4) return false;
        MIDIEventList list;
        MIDIEventPacket* mep = MIDIEventListInit(&list, kMIDIProtocol_2_0);
        mep = MIDIEventListAdd(&list, sizeof(list), mep, 0,
                               static_cast<ByteCount>(packet.word_count),
                               packet.words.data());
        OSStatus st = MIDISendEventList(out_port_, dest_, &list);
        return st == noErr;
    }

    bool is_open() const noexcept override { return open_; }

    // Called from the MIDIInputPort block. Bound to the same instance
    // for the lifetime of the OS port → fine for the block to capture
    // the raw pointer.
    void deliver_from_callback(const UmpPacket& p, double ts) {
        UmpReceiveCallback local;
        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            local = cb_;
        }
        if (local) local(p, ts);
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
        open_ = (src != 0 || dest != 0);
    }

private:
    UmpEndpointInfo info_;
    MIDIPortRef in_port_ = 0;
    MIDIEndpointRef src_ = 0;
    MIDIPortRef out_port_ = 0;
    MIDIEndpointRef dest_ = 0;
    bool open_ = false;
    std::mutex cb_mu_;
    UmpReceiveCallback cb_;
};

struct OsState {
    MIDIClientRef client = 0;
    std::mutex mu;
    std::unordered_map<std::string, std::unique_ptr<CoreMidiUmpEndpoint>> endpoints;
};

UmpEndpointInfo info_for_endpoint(MIDIEndpointRef ep, bool is_source) {
    UmpEndpointInfo info;
    SInt32 unique_id = 0;
    MIDIObjectGetIntegerProperty(ep, kMIDIPropertyUniqueID, &unique_id);
    info.id = std::to_string(unique_id);

    CFStringRef name = nullptr;
    MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name);
    if (name) {
        char buf[256];
        CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
        info.name = buf;
        CFRelease(name);
    }
    info.direction.can_receive = is_source;
    info.direction.can_send = !is_source;
    return info;
}

bool os_init(const UmpSessionConfig& cfg, void** out_state) {
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

    // Merge sources and destinations by unique-id so a paired physical
    // device shows up once with both direction flags set.
    std::unordered_map<std::string, UmpEndpointInfo> by_id;

    ItemCount src_count = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < src_count; ++i) {
        auto info = info_for_endpoint(MIDIGetSource(i), /*is_source=*/true);
        auto it = by_id.find(info.id);
        if (it == by_id.end()) {
            by_id.emplace(info.id, std::move(info));
        } else {
            it->second.direction.can_receive = true;
        }
    }
    ItemCount dest_count = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < dest_count; ++i) {
        auto info = info_for_endpoint(MIDIGetDestination(i), /*is_source=*/false);
        auto it = by_id.find(info.id);
        if (it == by_id.end()) {
            by_id.emplace(info.id, std::move(info));
        } else {
            it->second.direction.can_send = true;
            if (it->second.name.empty()) it->second.name = std::move(info.name);
        }
    }
    out.reserve(by_id.size());
    for (auto& [_, v] : by_id) out.push_back(std::move(v));
    return out;
}

UmpEndpoint* os_open(void* opaque, const std::string& id, UmpOpenStatus* status) {
    auto* state = static_cast<OsState*>(opaque);
    if (!state || !state->client) {
        if (status) *status = UmpOpenStatus::OsBackendUnavailable;
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(state->mu);
        auto it = state->endpoints.find(id);
        if (it != state->endpoints.end()) {
            if (status) *status = UmpOpenStatus::Ok;
            return it->second.get();
        }
    }

    // Resolve id → endpoint refs.
    SInt32 target_uid = 0;
    try { target_uid = static_cast<SInt32>(std::stol(id)); }
    catch (...) {
        if (status) *status = UmpOpenStatus::NotFound;
        return nullptr;
    }
    MIDIEndpointRef src = 0, dest = 0;
    ItemCount src_count = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < src_count; ++i) {
        MIDIEndpointRef e = MIDIGetSource(i);
        SInt32 uid = 0;
        MIDIObjectGetIntegerProperty(e, kMIDIPropertyUniqueID, &uid);
        if (uid == target_uid) { src = e; break; }
    }
    ItemCount dest_count = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < dest_count; ++i) {
        MIDIEndpointRef e = MIDIGetDestination(i);
        SInt32 uid = 0;
        MIDIObjectGetIntegerProperty(e, kMIDIPropertyUniqueID, &uid);
        if (uid == target_uid) { dest = e; break; }
    }
    if (!src && !dest) {
        if (status) *status = UmpOpenStatus::NotFound;
        return nullptr;
    }

    UmpEndpointInfo info;
    info.id = id;
    if (src) {
        auto si = info_for_endpoint(src, /*is_source=*/true);
        info.name = std::move(si.name);
        info.direction.can_receive = true;
    }
    if (dest) {
        auto di = info_for_endpoint(dest, /*is_source=*/false);
        if (info.name.empty()) info.name = std::move(di.name);
        info.direction.can_send = true;
    }

    // Construct the endpoint up front so the block can capture its
    // stable raw pointer. The session owns the unique_ptr.
    auto ep = std::make_unique<CoreMidiUmpEndpoint>(std::move(info));
    CoreMidiUmpEndpoint* ep_raw = ep.get();

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
                        const double ts =
                            static_cast<double>(packet->timeStamp) / 1e9;
                        ep_raw->deliver_from_callback(p, ts);
                        idx += words;
                    }
                    packet = MIDIEventPacketNext(packet);
                }
            });
        if (st == noErr && in_port) {
            MIDIPortConnectSource(in_port, src, nullptr);
        }
    }
    if (dest) {
        MIDIOutputPortCreate(state->client, CFSTR("Pulp UMP Out"), &out_port);
    }
    ep->set_ports(in_port, src, out_port, dest);

    UmpEndpoint* out_ptr = ep.get();
    {
        std::lock_guard<std::mutex> lk(state->mu);
        state->endpoints[id] = std::move(ep);
    }
    if (status) *status = UmpOpenStatus::Ok;
    return out_ptr;
}

struct BackendRegistrar {
    BackendRegistrar() {
        ump_os::OsBackendVTable v;
        v.init = &os_init;
        v.shutdown = &os_shutdown;
        v.enumerate = &os_enumerate;
        v.open = &os_open;
        register_ump_os_backend(v);
    }
};
BackendRegistrar g_registrar;

} // namespace
} // namespace pulp::midi
