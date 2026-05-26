// Avahi backend for NetworkServiceDiscovery (Linux).
//
// Avahi is the standard Linux mDNS/DNS-SD stack — every mainstream
// distro ships `libavahi-client.so.3` from a host-installed
// `avahi-daemon`. We deliberately do NOT depend on the Avahi headers
// at build time: the build must succeed on a stock Linux box that has
// never heard of Avahi (developer laptops, minimal CI containers,
// embedded targets), and on macOS/Windows worktrees that compile this
// translation unit purely as a smoke for the symbol-resolver path.
//
// The strategy:
//   * The file is gated to `#if defined(__linux__)` so it produces an
//     empty TU on every other platform. The CMake glue adds it only on
//     Linux, but the guard lets `cmake --build` accidentally compile
//     it elsewhere without breaking.
//   * On Linux it uses `pulp::runtime::DynamicLibrary` to
//     `dlopen("libavahi-client.so.3", RTLD_LAZY)` and resolves every
//     Avahi symbol it needs at run-time. If the daemon isn't installed
//     `make_avahi_backend()` returns nullptr and
//     `install_default_backend()` falls through to the existing
//     "no mDNS available" path.
//   * We declare the Avahi types and enum values we actually use as a
//     minimal opaque/forward-declared mirror of the upstream public
//     headers (LGPL-2.1+ on Avahi itself, but plain function
//     signatures and enum constants are uncopyrightable interface
//     facts — same posture as Pulp's existing dl_shim wrappers).
//
// Threading model:
//   * Avahi uses a single-threaded reactor. We use
//     `avahi_threaded_poll_new()` so the loop runs on its own thread
//     and every callback is delivered on that thread without us
//     needing to drive `select()` by hand.
//   * Browse callbacks are chained into `avahi_service_resolver_new`,
//     just like the macOS path chains `DNSServiceBrowse` into
//     `DNSServiceResolve`, so consumers get hostname/port/TXT before
//     we fire `notify_service_found`.
//   * TXT records ride in via `avahi_string_list_new_from_array`
//     (encode) and `avahi_string_list_get_pair` (decode).
//
// What the runtime loader buys us (besides not hard-failing the
// build): if a user installs `avahi-daemon` later, the same Pulp
// binary picks up the backend on the next start — no recompile, no
// repackage.

#include <pulp/events/volume_detector.hpp>

#if defined(__linux__)

#include <pulp/runtime/dynamic_library.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::events {

namespace {

// ── Minimal Avahi public-API mirror ──────────────────────────────────
// These mirror the relevant fragments of <avahi-client/client.h>,
// <avahi-client/lookup.h>, <avahi-client/publish.h>,
// <avahi-common/thread-watch.h>, and <avahi-common/strlst.h>.
// Only the types/enums we touch are declared — opaque pointers stay
// opaque. We use C linkage for the function-pointer typedefs to match
// libavahi-client's ABI exactly.

struct AvahiClient;
struct AvahiThreadedPoll;
struct AvahiPoll;
struct AvahiServiceBrowser;
struct AvahiServiceResolver;
struct AvahiEntryGroup;
struct AvahiStringList;
struct AvahiAddress;

using AvahiIfIndex = int;
using AvahiProtocol = int;
constexpr AvahiIfIndex AVAHI_IF_UNSPEC = -1;
constexpr AvahiProtocol AVAHI_PROTO_UNSPEC = -1;

enum AvahiClientFlags { AVAHI_CLIENT_NO_FLAGS = 0 };
enum AvahiLookupFlags { AVAHI_LOOKUP_NO_LOOKUP_FLAGS = 0 };
enum AvahiPublishFlags { AVAHI_PUBLISH_NO_PUBLISH_FLAGS = 0 };

enum AvahiClientState {
    AVAHI_CLIENT_S_REGISTERING = 1,
    AVAHI_CLIENT_S_RUNNING = 2,
    AVAHI_CLIENT_S_COLLISION = 3,
    AVAHI_CLIENT_FAILURE = 100,
    AVAHI_CLIENT_CONNECTING = 101,
};

enum AvahiBrowserEvent {
    AVAHI_BROWSER_NEW = 0,
    AVAHI_BROWSER_REMOVE = 1,
    AVAHI_BROWSER_CACHE_EXHAUSTED = 2,
    AVAHI_BROWSER_ALL_FOR_NOW = 3,
    AVAHI_BROWSER_FAILURE = 4,
};

enum AvahiResolverEvent {
    AVAHI_RESOLVER_FOUND = 0,
    AVAHI_RESOLVER_FAILURE = 1,
};

extern "C" {
using AvahiClientCallback = void (*)(AvahiClient*, AvahiClientState,
                                     void* userdata);
using AvahiServiceBrowserCallback = void (*)(
    AvahiServiceBrowser*, AvahiIfIndex, AvahiProtocol, AvahiBrowserEvent,
    const char* name, const char* type, const char* domain,
    int /*AvahiLookupResultFlags*/, void* userdata);
using AvahiServiceResolverCallback = void (*)(
    AvahiServiceResolver*, AvahiIfIndex, AvahiProtocol, AvahiResolverEvent,
    const char* name, const char* type, const char* domain,
    const char* host_name, const AvahiAddress* a, uint16_t port,
    AvahiStringList* txt, int /*AvahiLookupResultFlags*/, void* userdata);
using AvahiEntryGroupCallback = void (*)(
    AvahiEntryGroup*, int /*AvahiEntryGroupState*/, void* userdata);
}  // extern "C"

// Function-pointer table for the symbols we resolve at run-time.
struct AvahiApi {
    // thread-watch
    AvahiThreadedPoll* (*threaded_poll_new)() = nullptr;
    void (*threaded_poll_free)(AvahiThreadedPoll*) = nullptr;
    AvahiPoll* (*threaded_poll_get)(AvahiThreadedPoll*) = nullptr;
    int (*threaded_poll_start)(AvahiThreadedPoll*) = nullptr;
    int (*threaded_poll_stop)(AvahiThreadedPoll*) = nullptr;
    void (*threaded_poll_lock)(AvahiThreadedPoll*) = nullptr;
    void (*threaded_poll_unlock)(AvahiThreadedPoll*) = nullptr;
    // client
    AvahiClient* (*client_new)(const AvahiPoll*, int /*flags*/,
                               AvahiClientCallback, void*, int*) = nullptr;
    void (*client_free)(AvahiClient*) = nullptr;
    // browse + resolve
    AvahiServiceBrowser* (*service_browser_new)(
        AvahiClient*, AvahiIfIndex, AvahiProtocol, const char*, const char*,
        int /*flags*/, AvahiServiceBrowserCallback, void*) = nullptr;
    int (*service_browser_free)(AvahiServiceBrowser*) = nullptr;
    AvahiServiceResolver* (*service_resolver_new)(
        AvahiClient*, AvahiIfIndex, AvahiProtocol, const char*, const char*,
        const char*, AvahiProtocol, int /*flags*/,
        AvahiServiceResolverCallback, void*) = nullptr;
    int (*service_resolver_free)(AvahiServiceResolver*) = nullptr;
    // register
    AvahiEntryGroup* (*entry_group_new)(AvahiClient*, AvahiEntryGroupCallback,
                                        void*) = nullptr;
    int (*entry_group_add_service_strlst)(
        AvahiEntryGroup*, AvahiIfIndex, AvahiProtocol, int /*flags*/,
        const char*, const char*, const char*, const char*, uint16_t,
        AvahiStringList*) = nullptr;
    int (*entry_group_commit)(AvahiEntryGroup*) = nullptr;
    int (*entry_group_reset)(AvahiEntryGroup*) = nullptr;
    int (*entry_group_free)(AvahiEntryGroup*) = nullptr;
    // string list (TXT)
    AvahiStringList* (*string_list_new_from_array)(const char**, int) = nullptr;
    void (*string_list_free)(AvahiStringList*) = nullptr;
    AvahiStringList* (*string_list_get_next)(AvahiStringList*) = nullptr;
    int (*string_list_get_pair)(AvahiStringList*, char**, char**,
                                size_t*) = nullptr;
    // address → string
    char* (*address_snprint)(char*, size_t, const AvahiAddress*) = nullptr;
    // libc free, looked up via the same handle (avahi_free is a thin
    // wrapper around libc free; we use libc free directly via
    // ::free below to avoid one more symbol lookup).
};

// Load every symbol; returns true only if all required ones resolved.
bool load_api(pulp::runtime::DynamicLibrary& lib, AvahiApi& api) {
    auto get = [&](auto& slot, const char* name) {
        slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(
            lib.find_symbol(name));
        return slot != nullptr;
    };

    bool ok = true;
    ok &= get(api.threaded_poll_new, "avahi_threaded_poll_new");
    ok &= get(api.threaded_poll_free, "avahi_threaded_poll_free");
    ok &= get(api.threaded_poll_get, "avahi_threaded_poll_get");
    ok &= get(api.threaded_poll_start, "avahi_threaded_poll_start");
    ok &= get(api.threaded_poll_stop, "avahi_threaded_poll_stop");
    ok &= get(api.threaded_poll_lock, "avahi_threaded_poll_lock");
    ok &= get(api.threaded_poll_unlock, "avahi_threaded_poll_unlock");
    ok &= get(api.client_new, "avahi_client_new");
    ok &= get(api.client_free, "avahi_client_free");
    ok &= get(api.service_browser_new, "avahi_service_browser_new");
    ok &= get(api.service_browser_free, "avahi_service_browser_free");
    ok &= get(api.service_resolver_new, "avahi_service_resolver_new");
    ok &= get(api.service_resolver_free, "avahi_service_resolver_free");
    ok &= get(api.entry_group_new, "avahi_entry_group_new");
    ok &= get(api.entry_group_add_service_strlst,
              "avahi_entry_group_add_service_strlst");
    ok &= get(api.entry_group_commit, "avahi_entry_group_commit");
    ok &= get(api.entry_group_reset, "avahi_entry_group_reset");
    ok &= get(api.entry_group_free, "avahi_entry_group_free");
    ok &= get(api.string_list_new_from_array,
              "avahi_string_list_new_from_array");
    ok &= get(api.string_list_free, "avahi_string_list_free");
    ok &= get(api.string_list_get_next, "avahi_string_list_get_next");
    ok &= get(api.string_list_get_pair, "avahi_string_list_get_pair");
    ok &= get(api.address_snprint, "avahi_address_snprint");
    return ok;
}

class AvahiBackend;

// Resolver context lives until the resolver fires. Owned by the
// resolver pointer; freed in resolve_callback after the result is
// dispatched.
struct ResolveContext {
    AvahiBackend* self = nullptr;
    std::string name;
    std::string type;
    std::string domain;
};

class AvahiBackend final : public NetworkServiceDiscovery::Backend {
public:
    AvahiBackend(std::unique_ptr<pulp::runtime::DynamicLibrary> lib,
                 AvahiApi api)
        : lib_(std::move(lib)), api_(api) {
        threaded_poll_ = api_.threaded_poll_new();
        if (!threaded_poll_) return;
        int error = 0;
        client_ = api_.client_new(
            api_.threaded_poll_get(threaded_poll_),
            AVAHI_CLIENT_NO_FLAGS,
            &AvahiBackend::client_callback,
            this,
            &error);
        if (!client_) {
            api_.threaded_poll_free(threaded_poll_);
            threaded_poll_ = nullptr;
            return;
        }
        api_.threaded_poll_start(threaded_poll_);
    }

    ~AvahiBackend() override {
        stop();
        unregister_service();
        if (threaded_poll_) api_.threaded_poll_stop(threaded_poll_);
        if (client_) api_.client_free(client_);
        if (threaded_poll_) api_.threaded_poll_free(threaded_poll_);
        // ResolveContexts are deleted inside resolve_callback once the
        // resolver fires; if we're tearing down before any callback we
        // accept that ~AvahiBackend before client_free leaves them in
        // limbo. client_free + resolver_free invalidate them so the
        // ABI guarantees no further callbacks.
    }

    bool valid() const { return client_ != nullptr; }

    void browse(std::string_view service_type,
                NetworkServiceDiscovery& owner) override {
        if (!client_) return;
        // Tear down any prior browse so callers can browse() again.
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            // Snapshot for use after unlock.
        }
        stop_browse();

        std::lock_guard<std::mutex> lock(state_mutex_);
        owner_ = &owner;
        browse_type_.assign(service_type);

        api_.threaded_poll_lock(threaded_poll_);
        browse_ = api_.service_browser_new(
            client_,
            AVAHI_IF_UNSPEC,
            AVAHI_PROTO_UNSPEC,
            browse_type_.c_str(),
            /*domain*/ nullptr,
            AVAHI_LOOKUP_NO_LOOKUP_FLAGS,
            &AvahiBackend::browse_callback,
            this);
        api_.threaded_poll_unlock(threaded_poll_);
        if (!browse_) owner_ = nullptr;
    }

    void stop() override {
        stop_browse();
    }

    bool register_service(std::string_view name,
                          std::string_view type,
                          uint16_t port) override {
        return register_service(name, type, port,
                                NetworkServiceDiscovery::TxtRecords{});
    }

    bool register_service(std::string_view name,
                          std::string_view type,
                          uint16_t port,
                          const NetworkServiceDiscovery::TxtRecords& txt) override {
        if (!client_) return false;
        unregister_service();

        // Encode TXT records as a NUL-terminated "key=value" array,
        // matching avahi_string_list_new_from_array's contract.
        std::vector<std::string> kv;
        kv.reserve(txt.size());
        for (const auto& [k, v] : txt) kv.push_back(k + "=" + v);
        std::vector<const char*> kv_ptrs;
        kv_ptrs.reserve(kv.size());
        for (const auto& s : kv) kv_ptrs.push_back(s.c_str());
        AvahiStringList* strlst = api_.string_list_new_from_array(
            kv_ptrs.empty() ? nullptr : kv_ptrs.data(),
            static_cast<int>(kv_ptrs.size()));

        std::string name_str(name);
        std::string type_str(type);

        api_.threaded_poll_lock(threaded_poll_);
        AvahiEntryGroup* group = api_.entry_group_new(
            client_,
            &AvahiBackend::entry_group_callback,
            this);
        bool ok = false;
        if (group) {
            const int add_result = api_.entry_group_add_service_strlst(
                group,
                AVAHI_IF_UNSPEC,
                AVAHI_PROTO_UNSPEC,
                AVAHI_PUBLISH_NO_PUBLISH_FLAGS,
                name_str.c_str(),
                type_str.c_str(),
                /*domain*/ nullptr,
                /*host*/ nullptr,
                port,
                strlst);
            if (add_result == 0) {
                ok = (api_.entry_group_commit(group) == 0);
            }
            if (!ok) {
                api_.entry_group_free(group);
                group = nullptr;
            }
        }
        api_.threaded_poll_unlock(threaded_poll_);

        if (strlst) api_.string_list_free(strlst);

        if (ok) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            entry_group_ = group;
        }
        return ok;
    }

    void unregister_service() override {
        AvahiEntryGroup* group = nullptr;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            group = entry_group_;
            entry_group_ = nullptr;
        }
        if (group) {
            api_.threaded_poll_lock(threaded_poll_);
            api_.entry_group_reset(group);
            api_.entry_group_free(group);
            api_.threaded_poll_unlock(threaded_poll_);
        }
    }

private:
    void stop_browse() {
        AvahiServiceBrowser* b = nullptr;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            b = browse_;
            browse_ = nullptr;
            owner_ = nullptr;
        }
        if (b && threaded_poll_) {
            api_.threaded_poll_lock(threaded_poll_);
            api_.service_browser_free(b);
            api_.threaded_poll_unlock(threaded_poll_);
        }
    }

    static void client_callback(AvahiClient* /*c*/, AvahiClientState /*state*/,
                                void* /*userdata*/) {
        // State transitions are interesting for logging/diagnostics
        // but the backend is happy to operate without acting on them;
        // browse + register both check `client_` before issuing calls.
    }

    static void entry_group_callback(AvahiEntryGroup* /*g*/, int /*state*/,
                                     void* /*userdata*/) {
        // Collision / failure handling is a future improvement. For
        // now the synchronous return value of entry_group_commit is
        // the publish-or-not signal we expose to the dispatcher.
    }

    static void browse_callback(
        AvahiServiceBrowser* /*b*/, AvahiIfIndex iface, AvahiProtocol proto,
        AvahiBrowserEvent event, const char* name, const char* type,
        const char* domain, int /*flags*/, void* userdata) {
        auto* self = static_cast<AvahiBackend*>(userdata);
        if (!self) return;
        NetworkServiceDiscovery* owner = nullptr;
        {
            std::lock_guard<std::mutex> lock(self->state_mutex_);
            owner = self->owner_;
        }
        if (!owner || !name || !type) return;

        if (event == AVAHI_BROWSER_REMOVE) {
            NetworkServiceDiscovery::Service svc;
            svc.name = name;
            svc.type = type;
            owner->notify_service_lost(svc);
            return;
        }
        if (event != AVAHI_BROWSER_NEW) return;

        // Resolve to get hostname / port / TXT. The resolver
        // delivers exactly one callback on the same poll thread; we
        // free the resolver and the heap context inside it.
        auto* ctx = new ResolveContext;
        ctx->self = self;
        ctx->name = name;
        ctx->type = type;
        ctx->domain = domain ? domain : "";

        AvahiServiceResolver* r = self->api_.service_resolver_new(
            self->client_,
            iface,
            proto,
            name,
            type,
            domain,
            AVAHI_PROTO_UNSPEC,
            AVAHI_LOOKUP_NO_LOOKUP_FLAGS,
            &AvahiBackend::resolve_callback,
            ctx);
        if (!r) delete ctx;
    }

    static void resolve_callback(
        AvahiServiceResolver* r, AvahiIfIndex /*iface*/, AvahiProtocol /*proto*/,
        AvahiResolverEvent event, const char* /*name*/, const char* /*type*/,
        const char* /*domain*/, const char* host_name, const AvahiAddress* a,
        uint16_t port, AvahiStringList* txt, int /*flags*/, void* userdata) {
        auto ctx_holder = std::unique_ptr<ResolveContext>(
            static_cast<ResolveContext*>(userdata));
        if (!ctx_holder) return;
        AvahiBackend* self = ctx_holder->self;
        if (event == AVAHI_RESOLVER_FOUND && self) {
            NetworkServiceDiscovery* owner = nullptr;
            {
                std::lock_guard<std::mutex> lock(self->state_mutex_);
                owner = self->owner_;
            }
            if (owner) {
                NetworkServiceDiscovery::Service svc;
                svc.name = ctx_holder->name;
                svc.type = ctx_holder->type;
                svc.hostname = host_name ? host_name : "";
                svc.port = port;
                if (a) {
                    char buf[64] = {0};
                    self->api_.address_snprint(buf, sizeof(buf), a);
                    svc.address = buf;
                }
                // Decode TXT.
                for (AvahiStringList* it = txt; it != nullptr;
                     it = self->api_.string_list_get_next(it)) {
                    char* key = nullptr;
                    char* value = nullptr;
                    size_t vlen = 0;
                    if (self->api_.string_list_get_pair(it, &key, &value, &vlen) == 0) {
                        if (key) {
                            std::string k(key);
                            std::string v;
                            if (value) v.assign(value, value + vlen);
                            svc.txt_records.emplace(std::move(k), std::move(v));
                            ::free(key);
                            if (value) ::free(value);
                        }
                    }
                }
                owner->notify_service_found(svc);
            }
        }
        if (self && r) self->api_.service_resolver_free(r);
    }

    std::unique_ptr<pulp::runtime::DynamicLibrary> lib_;
    AvahiApi api_;
    AvahiThreadedPoll* threaded_poll_ = nullptr;
    AvahiClient* client_ = nullptr;

    std::mutex state_mutex_;
    NetworkServiceDiscovery* owner_ = nullptr;
    std::string browse_type_;
    AvahiServiceBrowser* browse_ = nullptr;
    AvahiEntryGroup* entry_group_ = nullptr;
};

}  // namespace

// Returns nullptr when libavahi-client.so.3 is not installed or the
// daemon is unreachable. install_default_backend then reports
// "no mDNS available" to the caller, matching the contract on
// macOS / iOS when Bonjour is disabled.
std::unique_ptr<NetworkServiceDiscovery::Backend> make_avahi_backend() {
    auto lib = std::make_unique<pulp::runtime::DynamicLibrary>();
    if (!lib->open("libavahi-client.so.3")) {
        // Try the unversioned name too; some distros only ship the
        // development symlink at the explicit `.so.3` path.
        if (!lib->open("libavahi-client.so")) return nullptr;
    }
    AvahiApi api;
    if (!load_api(*lib, api)) return nullptr;

    auto backend = std::make_unique<AvahiBackend>(std::move(lib), api);
    if (!backend->valid()) return nullptr;
    return backend;
}

}  // namespace pulp::events

#endif  // __linux__
