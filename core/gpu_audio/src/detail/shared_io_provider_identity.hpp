#pragma once

#include <pulp/runtime/crypto.hpp>

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
    std::string native_runtime_revision;
};

inline std::string shared_io_provider_receipt_digest(const SharedIoProviderIdentity& identity) {
    const auto receipt = std::string{"pulp.gpu-audio.provider.v1\n"} +
                         "provider_revision=" + identity.provider_revision + "\n" +
                         "adapter_name=" + identity.adapter_name + "\n" +
                         "adapter_backend=" + identity.adapter_backend + "\n" +
                         "adapter_vendor_id=" + std::to_string(identity.adapter_vendor_id) +
                         "\nadapter_device_id=" + std::to_string(identity.adapter_device_id) +
                         "\nnative_runtime_name=" + identity.native_runtime_name +
                         "\nnative_runtime_backend=" + identity.native_runtime_backend +
                         "\nnative_runtime_revision=" + identity.native_runtime_revision + "\n";
    return pulp::runtime::sha256_hex(receipt);
}

} // namespace pulp::gpu_audio::detail
