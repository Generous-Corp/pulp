#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pulp::gpu_audio::detail {

// Identity observed directly from the native graphics runtime. This is a
// private qualification seam: adapter metadata and environment labels never
// populate it. The revision is owned by the native observer (for Metal this
// is the selected device's immutable registry identity), while the Dawn
// provider revision remains separate in SharedIoProviderIdentity.
struct NativeRuntimeIdentity {
    bool authenticated = false;
    std::string name;
    std::string backend;
    std::string revision;
    std::uint64_t registry_id = 0;
};

// The observer receives the selected Dawn adapter's observed identity and
// must prove that the native device it reports is the same adapter. Platforms
// without a first-party observer return an unauthenticated value.
NativeRuntimeIdentity observe_native_metal(std::string_view selected_name,
                                           std::uint64_t selected_vendor_id,
                                           std::uint64_t selected_device_id) noexcept;

// Resolve the native device that Dawn actually created. The opaque handle is
// a WGPUDevice so this private header stays free of Dawn and Objective-C
// types; implementations without a first-party observer return unavailable.
NativeRuntimeIdentity observe_native_metal_device(void* dawn_device, std::string_view selected_name,
                                                  std::uint64_t selected_vendor_id,
                                                  std::uint64_t selected_device_id) noexcept;

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
