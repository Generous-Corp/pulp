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

// How far a provider identity reaches. A provider can authenticate itself and
// its adapter while no independent source for the native runtime exists at
// its boundary; that is an unavailable qualification, not a failed one, and a
// physical qualification reports it as a typed skip.
enum class SharedIoProviderIdentityGate : std::uint8_t {
    Authenticated,
    NativeRuntimeUnavailable,
    Unauthenticated,
};

inline SharedIoProviderIdentityGate
gate_shared_io_provider_identity(const SharedIoProviderIdentity& identity) noexcept {
    if (!identity.authenticated)
        return SharedIoProviderIdentityGate::Unauthenticated;
    return identity.native_runtime_authenticated
               ? SharedIoProviderIdentityGate::Authenticated
               : SharedIoProviderIdentityGate::NativeRuntimeUnavailable;
}

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
