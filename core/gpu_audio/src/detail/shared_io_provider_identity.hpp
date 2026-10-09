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
    bool native_runtime_authenticated = false;
    std::string native_runtime_name;
    std::string native_runtime_backend;
};

// Provider-owned execution capabilities.  These are deliberately semantic
// facts used by the private realtime seam; they contain no Dawn, WebGPU, or
// native-device types.  A provider that cannot authenticate a capability must
// leave it false so diagnostics and admission remain fail-closed.
struct SharedIoProviderCapabilities {
    bool imported_host_pointer = false;
    bool ordered_causal_state = false;
    bool completion_service = false;
    bool device_loss_recovery = false;
    bool gpu_timestamps = false;
};

} // namespace pulp::gpu_audio::detail
