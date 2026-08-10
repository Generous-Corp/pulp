#include <pulp/inspect/control_connection_admission.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace pulp::inspect {
namespace {

std::optional<std::string> random_admission_id() {
    const auto bytes = runtime::secure_random_bytes(16);
    if (!bytes)
        return std::nullopt;
    return "admission-" + runtime::hex_encode(*bytes);
}

bool valid_admission_id(std::string_view id) {
    constexpr std::string_view prefix = "admission-";
    constexpr std::size_t encoded_bytes = 32;
    return id.size() == prefix.size() + encoded_bytes && id.starts_with(prefix) &&
           std::all_of(id.begin() + static_cast<std::ptrdiff_t>(prefix.size()), id.end(),
                       [](char value) {
                           return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                       });
}

bool valid_peer(const ControlPeerEvidence& peer) {
    return !peer.user_id.empty() && peer.process_id > 0 && !peer.process_start_id.empty() &&
           !peer.executable_identity.empty() && !peer.publisher_id.empty();
}

bool valid_binding(const ControlPeerExpectation& expectation,
                   const ControlConnectionPrincipal& principal) {
    if (!valid_peer(expectation.evidence))
        return false;
    if (const auto* client = std::get_if<ControlClientConnectionPrincipal>(&principal))
        return expectation.evidence.role == ControlPeerRole::Client && bool(client->client_id);
    const auto* host = std::get_if<ControlHostConnectionPrincipal>(&principal);
    return host && expectation.evidence.role != ControlPeerRole::Client &&
           bool(host->registration_id);
}

} // namespace

struct ControlConnectionAdmissionStore::Impl {
    Impl(ControlConnectionAdmissionStoreConfig config_in, Clock clock_in)
        : config(std::move(config_in)), clock(std::move(clock_in)) {}

    void sweep_locked(std::chrono::steady_clock::time_point now) {
        for (auto it = admissions.begin(); it != admissions.end();) {
            if (it->second.expires_at <= now)
                it = admissions.erase(it);
            else
                ++it;
        }
    }

    ControlConnectionAdmissionStoreConfig config;
    Clock clock;
    mutable std::mutex mutex;
    std::unordered_map<std::string, ControlConnectionAdmission> admissions;
};

ControlConnectionAdmissionStore::ControlConnectionAdmissionStore(
    ControlConnectionAdmissionStoreConfig config, Clock clock)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(clock))) {}

ControlConnectionAdmissionStore::~ControlConnectionAdmissionStore() = default;

ControlConnectionAdmissionResult
ControlConnectionAdmissionStore::issue(ControlPeerExpectation expected_peer,
                                       ControlConnectionPrincipal principal) {
    if (!impl_->clock || impl_->config.maximum_admissions == 0 ||
        impl_->config.maximum_admissions > kControlMaximumConnectionAdmissions ||
        impl_->config.admission_ttl <= std::chrono::milliseconds::zero() ||
        impl_->config.admission_ttl > kControlMaximumConnectionAdmissionTtl ||
        !valid_binding(expected_peer, principal)) {
        return {.status = ControlConnectionAdmissionStatus::InvalidRequest};
    }

    std::lock_guard lock(impl_->mutex);
    const auto now = impl_->clock();
    impl_->sweep_locked(now);
    if (impl_->admissions.size() >= impl_->config.maximum_admissions)
        return {.status = ControlConnectionAdmissionStatus::ResourceExhausted};

    const auto admission_id = random_admission_id();
    if (!admission_id)
        return {.status = ControlConnectionAdmissionStatus::EntropyUnavailable};
    const auto expires_at = now + impl_->config.admission_ttl;
    auto [it, inserted] =
        impl_->admissions.emplace(*admission_id, ControlConnectionAdmission{
                                                     .admission_id = *admission_id,
                                                     .expected_peer = std::move(expected_peer),
                                                     .principal = std::move(principal),
                                                     .expires_at = expires_at,
                                                 });
    if (!inserted)
        return {.status = ControlConnectionAdmissionStatus::EntropyUnavailable};
    return {
        .status = ControlConnectionAdmissionStatus::Issued,
        .ticket = ControlConnectionAdmissionTicket{it->first, expires_at},
    };
}

std::optional<ControlConnectionAdmission>
ControlConnectionAdmissionStore::consume(std::string_view admission_id) {
    if (!valid_admission_id(admission_id) || !impl_->clock)
        return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto now = impl_->clock();
    impl_->sweep_locked(now);
    const auto it = impl_->admissions.find(std::string(admission_id));
    if (it == impl_->admissions.end())
        return std::nullopt;
    auto admission = std::move(it->second);
    impl_->admissions.erase(it);
    return admission;
}

void ControlConnectionAdmissionStore::sweep_expired() {
    if (!impl_->clock)
        return;
    std::lock_guard lock(impl_->mutex);
    impl_->sweep_locked(impl_->clock());
}

std::size_t ControlConnectionAdmissionStore::size() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->admissions.size();
}

} // namespace pulp::inspect
