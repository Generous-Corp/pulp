#pragma once

#include <cstdint>
#include <string>

namespace pulp::gpu_audio::detail {

// Host/quiescent-only identity of the provider that owns a prepared shared-I/O
// session. A false authenticated value is fail-closed and is never physical
// acceptance evidence.
struct SharedIoProviderIdentity {
    bool authenticated = false;
    std::string provider_revision;
    std::string adapter_name;
    std::string adapter_backend;
    std::uint64_t adapter_vendor_id = 0;
    std::uint64_t adapter_device_id = 0;
    std::string immutable_receipt_digest;
    bool native_runtime_authenticated = false;
    std::string native_runtime_name;
    std::string native_runtime_backend;
};

} // namespace pulp::gpu_audio::detail
