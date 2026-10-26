#pragma once

#include "shared_io_provider_identity.hpp"

#include <pulp/gpu_audio/gpu_audio_capability.hpp>

#include <string>

namespace pulp::gpu_audio::detail {

// GpuAudioProviderPool keys a held provider as "<kind>:<backend>:<revision>",
// all taken from the provider's own authenticated identity, so a provider of
// one kind is never lent to a request for another. Unauthenticated providers
// and providers of unknown kind get no key and are never pooled.
inline std::string provider_pool_key(const SharedIoProviderIdentity& identity) {
    if (!identity.authenticated || identity.provider == GpuAudioProvider::Unknown ||
        identity.adapter_backend.empty())
        return {};
    return std::to_string(static_cast<unsigned>(identity.provider)) + ":" +
           identity.adapter_backend + ":" + identity.provider_revision;
}

// A request whose revision is empty (a build that pins no revision) accepts
// any revision of the same kind and backend; otherwise the keys must be equal.
inline bool provider_pool_key_matches(const std::string& held, const std::string& requested) {
    if (held.empty() || requested.empty())
        return false;
    if (held == requested)
        return true;
    return requested.back() == ':' && held.size() > requested.size() &&
           held.compare(0, requested.size(), requested) == 0;
}

} // namespace pulp::gpu_audio::detail
