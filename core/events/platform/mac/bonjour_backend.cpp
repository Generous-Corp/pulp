// Bonjour / DNS-SD backend for NetworkServiceDiscovery.
//
// Apple ships <dns_sd.h> in /usr/include and exports the symbols from
// CoreServices.framework on macOS and (transitively) from libsystem on
// iOS. The header is C, so this file is a plain .cpp.
//
// Threading model:
//   - DNSServiceRegister and DNSServiceBrowse return a DNSServiceRef
//     and a callback function pointer. The reply socket is polled on a
//     dedicated worker thread spawned per ref.
//   - The worker blocks on DNSServiceProcessResult(), which delivers
//     callbacks on the calling thread. We forward each callback to the
//     owning NetworkServiceDiscovery via notify_service_found /
//     notify_service_lost, which mutate the dispatcher's cache and
//     fire the user's std::function handlers.
//   - The dispatcher itself is single-threaded (see the existing
//     #310/#314 codex notes in volume_detector.cpp); ServiceBrowser /
//     ServicePublisher own one NSD each, so there's no shared-state
//     race here. Apps that share an NSD across threads must serialize
//     externally — same contract as the rest of the dispatcher API.
//
// Lifecycle:
//   - stop() / unregister_service() call DNSServiceRefDeallocate on
//     the active ref. That tears down the kernel-level mDNS
//     registration and unblocks DNSServiceProcessResult on the worker,
//     which then exits. We join the worker before returning so the
//     destructor can safely run.

#include <pulp/events/volume_detector.hpp>

#if defined(__APPLE__)

#include <dns_sd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::events {

namespace {

class BonjourBackend final : public NetworkServiceDiscovery::Backend {
public:
    BonjourBackend() = default;
    ~BonjourBackend() override {
        stop();
        unregister_service();
    }

    void browse(std::string_view service_type,
                NetworkServiceDiscovery& owner) override {
        // Tear down any prior browse first so callers can safely
        // call browse() multiple times.
        stop_browse_locked();

        std::lock_guard<std::mutex> lock(state_mutex_);
        owner_ = &owner;
        browse_type_.assign(service_type);

        DNSServiceRef ref = nullptr;
        const DNSServiceErrorType err = DNSServiceBrowse(
            &ref,
            /*flags*/ 0,
            kDNSServiceInterfaceIndexAny,
            browse_type_.c_str(),
            /*domain*/ nullptr,
            &BonjourBackend::browse_callback,
            this);
        if (err != kDNSServiceErr_NoError || ref == nullptr) {
            owner_ = nullptr;
            return;
        }
        browse_ref_ = ref;
        browse_running_.store(true);
        browse_thread_ = std::thread(&BonjourBackend::process_loop, this,
                                     browse_ref_, &browse_running_);
    }

    void stop() override {
        stop_browse_locked();
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
        unregister_service();

        std::vector<unsigned char> txt_record;
        if (!encode_txt_record(txt, txt_record)) {
            return false;
        }

        std::string name_str(name);
        std::string type_str(type);

        DNSServiceRef ref = nullptr;
        const DNSServiceErrorType err = DNSServiceRegister(
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
            /*callBack*/ nullptr,
            /*context*/ nullptr);
        if (err != kDNSServiceErr_NoError || ref == nullptr) {
            return false;
        }

        std::lock_guard<std::mutex> lock(state_mutex_);
        register_ref_ = ref;
        register_running_.store(true);
        register_thread_ = std::thread(&BonjourBackend::process_loop, this,
                                       register_ref_, &register_running_);
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
        if (ref) DNSServiceRefDeallocate(ref);
        if (joiner.joinable()) joiner.join();
    }

private:
    void stop_browse_locked() {
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
        if (ref) DNSServiceRefDeallocate(ref);
        if (joiner.joinable()) joiner.join();
    }

    // Encode a TxtRecords map into the DNS-SD wire format: each
    // record is one length-prefixed byte string "key=value". Returns
    // false on overflow (key/value combined exceeds 255 bytes — the
    // RFC-6763 per-record cap).
    static bool encode_txt_record(
        const NetworkServiceDiscovery::TxtRecords& txt,
        std::vector<unsigned char>& out) {
        out.clear();
        for (const auto& [key, value] : txt) {
            std::string entry = key + "=" + value;
            if (entry.size() > 255) return false;
            out.push_back(static_cast<unsigned char>(entry.size()));
            out.insert(out.end(), entry.begin(), entry.end());
        }
        return true;
    }

    static void process_loop(BonjourBackend* self,
                             DNSServiceRef ref,
                             std::atomic<bool>* running) {
        // Wait for the reply socket to be readable, then deliver any
        // pending callbacks via DNSServiceProcessResult. select() with
        // a 250ms timeout lets us notice stop() reasonably quickly even
        // when no mDNS traffic is arriving.
        const int fd = DNSServiceRefSockFD(ref);
        if (fd < 0) return;
        while (running->load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 250 * 1000;
            const int n = ::select(fd + 1, &fds, nullptr, nullptr, &tv);
            if (n > 0 && FD_ISSET(fd, &fds)) {
                if (DNSServiceProcessResult(ref) != kDNSServiceErr_NoError) {
                    break;
                }
            } else if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
        }
        (void)self;
    }

    // DNSServiceBrowse delivers a callback per service-type discovery
    // event. We chain into DNSServiceResolve to get the hostname /
    // port / TXT records, which arrives on the same socket via a
    // second callback (resolve_callback below).
    static void DNSSD_API browse_callback(
        DNSServiceRef /*ref*/,
        DNSServiceFlags flags,
        uint32_t interface_index,
        DNSServiceErrorType err,
        const char* name,
        const char* type,
        const char* domain,
        void* context) {
        if (err != kDNSServiceErr_NoError) return;
        auto* self = static_cast<BonjourBackend*>(context);
        NetworkServiceDiscovery* owner = nullptr;
        {
            std::lock_guard<std::mutex> lock(self->state_mutex_);
            owner = self->owner_;
        }
        if (!owner || !name || !type) return;

        if ((flags & kDNSServiceFlagsAdd) == 0) {
            // Service went away. We don't have hostname/port for it,
            // but notify_service_lost matches by (name, type) only.
            NetworkServiceDiscovery::Service svc;
            svc.name = name;
            svc.type = type;
            owner->notify_service_lost(svc);
            return;
        }

        // Service appeared — start a resolve to get host/port/TXT.
        auto* resolve_ctx = new ResolveContext{self, std::string(name),
                                               std::string(type),
                                               std::string(domain ? domain : "")};
        DNSServiceRef resolve_ref = nullptr;
        const DNSServiceErrorType rerr = DNSServiceResolve(
            &resolve_ref,
            /*flags*/ 0,
            interface_index,
            name,
            type,
            domain,
            &BonjourBackend::resolve_callback,
            resolve_ctx);
        if (rerr != kDNSServiceErr_NoError || resolve_ref == nullptr) {
            delete resolve_ctx;
            return;
        }

        // Run the resolve in-line on this worker thread. The resolve
        // is expected to complete within a few ms once the local
        // mDNSResponder has the records cached; cap at 2s to avoid
        // leaking the worker if the network is being weird.
        const int fd = DNSServiceRefSockFD(resolve_ref);
        if (fd >= 0) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            timeval tv;
            tv.tv_sec = 2;
            tv.tv_usec = 0;
            if (::select(fd + 1, &fds, nullptr, nullptr, &tv) > 0
                && FD_ISSET(fd, &fds)) {
                DNSServiceProcessResult(resolve_ref);
            }
        }
        DNSServiceRefDeallocate(resolve_ref);
        delete resolve_ctx;
    }

    struct ResolveContext {
        BonjourBackend* self;
        std::string name;
        std::string type;
        std::string domain;
    };

    static void DNSSD_API resolve_callback(
        DNSServiceRef /*ref*/,
        DNSServiceFlags /*flags*/,
        uint32_t /*interface_index*/,
        DNSServiceErrorType err,
        const char* /*fullname*/,
        const char* host_target,
        uint16_t port_network_byte_order,
        uint16_t txt_len,
        const unsigned char* txt_record,
        void* context) {
        if (err != kDNSServiceErr_NoError) return;
        auto* ctx = static_cast<ResolveContext*>(context);
        if (!ctx || !ctx->self) return;
        BonjourBackend* self = ctx->self;
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

        // Best-effort hostname → A/AAAA lookup so the consumer gets a
        // dotted-quad address alongside the .local hostname. Skip on
        // failure — the hostname is still the authoritative handle.
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
            const std::string entry(reinterpret_cast<const char*>(txt + i), entry_len);
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

std::unique_ptr<NetworkServiceDiscovery::Backend> make_bonjour_backend() {
    return std::make_unique<BonjourBackend>();
}

}  // namespace pulp::events

#endif  // __APPLE__
