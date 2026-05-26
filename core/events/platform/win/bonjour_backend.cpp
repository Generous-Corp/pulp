// Bonjour SDK backend for NetworkServiceDiscovery (Windows).
//
// Apple ships the Bonjour SDK for Windows under a click-through Apple
// ID license; it installs `dnssd.dll` plus `dnssd.lib` and an SDK
// header. The DLL exports the exact same `DNSServiceBrowse` /
// `DNSServiceResolve` / `DNSServiceRegister` surface that lives in
// `<dns_sd.h>` on macOS, so this backend is structurally a copy of
// `platform/mac/bonjour_backend.cpp` with three differences:
//
//   1. The DLL is resolved at run-time via
//      `pulp::runtime::DynamicLibrary`, so Pulp builds and runs on
//      Windows boxes that have never had the Bonjour SDK installed —
//      `install_default_backend()` simply returns false there.
//   2. The fd-driven `select()` worker is replaced with a polling
//      thread that calls `DNSServiceProcessResult` whenever
//      `WSAEventSelect` signals readable on the ref's socket. This
//      keeps the threading model identical to macOS without depending
//      on POSIX `select` on Windows.
//   3. We intentionally avoid including `<dns_sd.h>` so the build
//      does not require the Bonjour SDK to be installed. The handful
//      of DNS-SD types and constants we touch are declared locally
//      as the documented public-ABI integers and opaque pointers.
//
// Layout-compatibility note: the DNS-SD ABI on Windows uses the same
// `__stdcall`/`__cdecl` calling conventions Apple's docs prescribe.
// Apple's reference impl declares the callbacks as `DNSSD_API` —
// which is `__stdcall` on MSVC, plain on everything else. We mirror
// that with `PULP_DNSSD_API` below so the ABI matches the DLL exports
// on every supported compiler.

#include <pulp/events/volume_detector.hpp>

#if defined(_WIN32)

#include <pulp/runtime/dynamic_library.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#define PULP_DNSSD_API __stdcall
#else
#define PULP_DNSSD_API
#endif

namespace pulp::events {

namespace {

// ── Minimal DNS-SD ABI mirror ────────────────────────────────────────
// These mirror the relevant fragments of <dns_sd.h> so we can call
// `dnssd.dll` without depending on the Bonjour SDK header at build
// time. The integer values and signatures are public-ABI facts.

using DNSServiceFlags = uint32_t;
using DNSServiceErrorType = int32_t;
using dnssd_sock_t = SOCKET;

struct _DNSServiceRef_t;
using DNSServiceRef = _DNSServiceRef_t*;

constexpr DNSServiceErrorType kDNSServiceErr_NoError = 0;
constexpr DNSServiceFlags kDNSServiceFlagsAdd = 0x2;
constexpr uint32_t kDNSServiceInterfaceIndexAny = 0;

extern "C" {
using DNSServiceBrowseReply = void(PULP_DNSSD_API*)(
    DNSServiceRef, DNSServiceFlags, uint32_t /*interfaceIndex*/,
    DNSServiceErrorType, const char* /*serviceName*/,
    const char* /*regtype*/, const char* /*replyDomain*/,
    void* /*context*/);

using DNSServiceResolveReply = void(PULP_DNSSD_API*)(
    DNSServiceRef, DNSServiceFlags, uint32_t /*interfaceIndex*/,
    DNSServiceErrorType, const char* /*fullname*/,
    const char* /*hosttarget*/, uint16_t /*port (network order)*/,
    uint16_t /*txtLen*/, const unsigned char* /*txtRecord*/,
    void* /*context*/);

using DNSServiceRegisterReply = void(PULP_DNSSD_API*)(
    DNSServiceRef, DNSServiceFlags, DNSServiceErrorType,
    const char* /*name*/, const char* /*regtype*/, const char* /*domain*/,
    void* /*context*/);
}  // extern "C"

struct DnsSdApi {
    DNSServiceErrorType(PULP_DNSSD_API* DNSServiceBrowse)(
        DNSServiceRef*, DNSServiceFlags, uint32_t, const char*, const char*,
        DNSServiceBrowseReply, void*) = nullptr;
    DNSServiceErrorType(PULP_DNSSD_API* DNSServiceResolve)(
        DNSServiceRef*, DNSServiceFlags, uint32_t, const char*, const char*,
        const char*, DNSServiceResolveReply, void*) = nullptr;
    DNSServiceErrorType(PULP_DNSSD_API* DNSServiceRegister)(
        DNSServiceRef*, DNSServiceFlags, uint32_t, const char*, const char*,
        const char*, const char*, uint16_t, uint16_t,
        const void* /*txtRecord*/, DNSServiceRegisterReply,
        void*) = nullptr;
    DNSServiceErrorType(PULP_DNSSD_API* DNSServiceProcessResult)(
        DNSServiceRef) = nullptr;
    void(PULP_DNSSD_API* DNSServiceRefDeallocate)(DNSServiceRef) = nullptr;
    dnssd_sock_t(PULP_DNSSD_API* DNSServiceRefSockFD)(DNSServiceRef) = nullptr;
};

bool load_api(pulp::runtime::DynamicLibrary& lib, DnsSdApi& api) {
    auto get = [&](auto& slot, const char* name) {
        slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(
            lib.find_symbol(name));
        return slot != nullptr;
    };
    bool ok = true;
    ok &= get(api.DNSServiceBrowse, "DNSServiceBrowse");
    ok &= get(api.DNSServiceResolve, "DNSServiceResolve");
    ok &= get(api.DNSServiceRegister, "DNSServiceRegister");
    ok &= get(api.DNSServiceProcessResult, "DNSServiceProcessResult");
    ok &= get(api.DNSServiceRefDeallocate, "DNSServiceRefDeallocate");
    ok &= get(api.DNSServiceRefSockFD, "DNSServiceRefSockFD");
    return ok;
}

class WindowsBonjourBackend final
    : public NetworkServiceDiscovery::Backend {
public:
    WindowsBonjourBackend(std::unique_ptr<pulp::runtime::DynamicLibrary> lib,
                          DnsSdApi api)
        : lib_(std::move(lib)), api_(api) {}

    ~WindowsBonjourBackend() override {
        stop();
        unregister_service();
    }

    void browse(std::string_view service_type,
                NetworkServiceDiscovery& owner) override {
        stop_browse();

        std::lock_guard<std::mutex> lock(state_mutex_);
        owner_ = &owner;
        browse_type_.assign(service_type);

        DNSServiceRef ref = nullptr;
        const DNSServiceErrorType err = api_.DNSServiceBrowse(
            &ref,
            /*flags*/ 0,
            kDNSServiceInterfaceIndexAny,
            browse_type_.c_str(),
            /*domain*/ nullptr,
            &WindowsBonjourBackend::browse_callback,
            this);
        if (err != kDNSServiceErr_NoError || ref == nullptr) {
            owner_ = nullptr;
            return;
        }
        browse_ref_ = ref;
        browse_running_.store(true);
        browse_thread_ = std::thread(&WindowsBonjourBackend::process_loop,
                                     this, browse_ref_, &browse_running_);
    }

    void stop() override { stop_browse(); }

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
        unregister_service();

        std::vector<unsigned char> txt_record;
        if (!encode_txt_record(txt, txt_record)) return false;

        std::string name_str(name);
        std::string type_str(type);

        DNSServiceRef ref = nullptr;
        const DNSServiceErrorType err = api_.DNSServiceRegister(
            &ref,
            /*flags*/ 0,
            kDNSServiceInterfaceIndexAny,
            name_str.c_str(),
            type_str.c_str(),
            /*domain*/ nullptr,
            /*host*/ nullptr,
            htons(port),
            static_cast<uint16_t>(txt_record.size()),
            txt_record.empty() ? nullptr : txt_record.data(),
            /*callback*/ nullptr,
            /*context*/ nullptr);
        if (err != kDNSServiceErr_NoError || ref == nullptr) return false;

        std::lock_guard<std::mutex> lock(state_mutex_);
        register_ref_ = ref;
        register_running_.store(true);
        register_thread_ = std::thread(&WindowsBonjourBackend::process_loop,
                                       this, register_ref_, &register_running_);
        return true;
    }

    void unregister_service() override {
        DNSServiceRef ref = nullptr;
        std::thread joiner;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            ref = register_ref_;
            register_ref_ = nullptr;
            register_running_.store(false);
            joiner = std::move(register_thread_);
        }
        if (ref) api_.DNSServiceRefDeallocate(ref);
        if (joiner.joinable()) joiner.join();
    }

private:
    void stop_browse() {
        DNSServiceRef ref = nullptr;
        std::thread joiner;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            ref = browse_ref_;
            browse_ref_ = nullptr;
            browse_running_.store(false);
            joiner = std::move(browse_thread_);
            owner_ = nullptr;
        }
        if (ref) api_.DNSServiceRefDeallocate(ref);
        if (joiner.joinable()) joiner.join();
    }

    static bool encode_txt_record(
        const NetworkServiceDiscovery::TxtRecords& txt,
        std::vector<unsigned char>& out) {
        out.clear();
        for (const auto& [k, v] : txt) {
            std::string entry = k + "=" + v;
            if (entry.size() > 255) return false;
            out.push_back(static_cast<unsigned char>(entry.size()));
            out.insert(out.end(), entry.begin(), entry.end());
        }
        return true;
    }

    static void process_loop(WindowsBonjourBackend* self,
                             DNSServiceRef ref,
                             std::atomic<bool>* running) {
        // Mirror the macOS strategy: block on select() with a 250ms
        // timeout so stop() can unwedge us promptly via
        // DNSServiceRefDeallocate.
        const SOCKET fd = self->api_.DNSServiceRefSockFD(ref);
        if (fd == INVALID_SOCKET) return;
        while (running->load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 250 * 1000;
            const int n = ::select(0, &fds, nullptr, nullptr, &tv);
            if (n > 0 && FD_ISSET(fd, &fds)) {
                if (self->api_.DNSServiceProcessResult(ref) != kDNSServiceErr_NoError)
                    break;
            } else if (n == SOCKET_ERROR) {
                break;
            }
        }
    }

    struct ResolveContext {
        WindowsBonjourBackend* self = nullptr;
        std::string name;
        std::string type;
        std::string domain;
    };

    static void PULP_DNSSD_API browse_callback(
        DNSServiceRef /*ref*/, DNSServiceFlags flags, uint32_t interface_index,
        DNSServiceErrorType err, const char* name, const char* type,
        const char* domain, void* context) {
        if (err != kDNSServiceErr_NoError) return;
        auto* self = static_cast<WindowsBonjourBackend*>(context);
        NetworkServiceDiscovery* owner = nullptr;
        {
            std::lock_guard<std::mutex> lock(self->state_mutex_);
            owner = self->owner_;
        }
        if (!owner || !name || !type) return;

        if ((flags & kDNSServiceFlagsAdd) == 0) {
            NetworkServiceDiscovery::Service svc;
            svc.name = name;
            svc.type = type;
            owner->notify_service_lost(svc);
            return;
        }

        auto* ctx = new ResolveContext{self, std::string(name),
                                       std::string(type),
                                       std::string(domain ? domain : "")};
        DNSServiceRef resolve_ref = nullptr;
        const DNSServiceErrorType rerr = self->api_.DNSServiceResolve(
            &resolve_ref,
            /*flags*/ 0,
            interface_index,
            name,
            type,
            domain,
            &WindowsBonjourBackend::resolve_callback,
            ctx);
        if (rerr != kDNSServiceErr_NoError || resolve_ref == nullptr) {
            delete ctx;
            return;
        }

        const SOCKET fd = self->api_.DNSServiceRefSockFD(resolve_ref);
        if (fd != INVALID_SOCKET) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            timeval tv;
            tv.tv_sec = 2;
            tv.tv_usec = 0;
            if (::select(0, &fds, nullptr, nullptr, &tv) > 0 && FD_ISSET(fd, &fds))
                self->api_.DNSServiceProcessResult(resolve_ref);
        }
        self->api_.DNSServiceRefDeallocate(resolve_ref);
        delete ctx;
    }

    static void PULP_DNSSD_API resolve_callback(
        DNSServiceRef /*ref*/, DNSServiceFlags /*flags*/,
        uint32_t /*interface_index*/, DNSServiceErrorType err,
        const char* /*fullname*/, const char* host_target,
        uint16_t port_network_byte_order, uint16_t txt_len,
        const unsigned char* txt_record, void* context) {
        if (err != kDNSServiceErr_NoError) return;
        auto* ctx = static_cast<ResolveContext*>(context);
        if (!ctx || !ctx->self) return;
        WindowsBonjourBackend* self = ctx->self;
        NetworkServiceDiscovery* owner = nullptr;
        {
            std::lock_guard<std::mutex> lock(self->state_mutex_);
            owner = self->owner_;
        }
        if (!owner) return;

        NetworkServiceDiscovery::Service svc;
        svc.name = ctx->name;
        svc.type = ctx->type;
        svc.hostname = host_target ? host_target : "";
        svc.port = ntohs(port_network_byte_order);
        svc.txt_records = decode_txt_record(txt_record, txt_len);

        if (!svc.hostname.empty()) {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* res = nullptr;
            if (::getaddrinfo(svc.hostname.c_str(), nullptr, &hints, &res) == 0
                && res != nullptr) {
                char buf[INET6_ADDRSTRLEN] = {0};
                void* addr_ptr = nullptr;
                if (res->ai_family == AF_INET) {
                    addr_ptr = &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
                } else if (res->ai_family == AF_INET6) {
                    addr_ptr = &reinterpret_cast<sockaddr_in6*>(res->ai_addr)->sin6_addr;
                }
                if (addr_ptr
                    && ::inet_ntop(res->ai_family, addr_ptr, buf, sizeof(buf)) != nullptr) {
                    svc.address = buf;
                }
                ::freeaddrinfo(res);
            }
        }

        owner->notify_service_found(svc);
    }

    static NetworkServiceDiscovery::TxtRecords decode_txt_record(
        const unsigned char* txt, uint16_t len) {
        NetworkServiceDiscovery::TxtRecords out;
        if (!txt || len == 0) return out;
        uint16_t i = 0;
        while (i < len) {
            const uint8_t entry_len = txt[i++];
            if (i + entry_len > len) break;
            const std::string entry(
                reinterpret_cast<const char*>(txt + i), entry_len);
            i += entry_len;
            const auto eq = entry.find('=');
            if (eq == std::string::npos) {
                out.emplace(entry, std::string{});
            } else {
                out.emplace(entry.substr(0, eq), entry.substr(eq + 1));
            }
        }
        return out;
    }

    std::unique_ptr<pulp::runtime::DynamicLibrary> lib_;
    DnsSdApi api_;

    std::mutex state_mutex_;
    NetworkServiceDiscovery* owner_ = nullptr;
    std::string browse_type_;

    DNSServiceRef browse_ref_ = nullptr;
    std::atomic<bool> browse_running_{false};
    std::thread browse_thread_;

    DNSServiceRef register_ref_ = nullptr;
    std::atomic<bool> register_running_{false};
    std::thread register_thread_;
};

}  // namespace

// Returns nullptr when dnssd.dll is not present (no Bonjour SDK
// installed). install_default_backend then reports "no mDNS available"
// to the caller — same contract as Linux when avahi-daemon isn't
// running.
std::unique_ptr<NetworkServiceDiscovery::Backend> make_windows_bonjour_backend() {
    auto lib = std::make_unique<pulp::runtime::DynamicLibrary>();
    if (!lib->open("dnssd.dll")) return nullptr;
    DnsSdApi api;
    if (!load_api(*lib, api)) return nullptr;
    return std::make_unique<WindowsBonjourBackend>(std::move(lib), api);
}

}  // namespace pulp::events

#endif  // _WIN32
