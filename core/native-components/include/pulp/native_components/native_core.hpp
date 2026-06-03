// pulp/native_components/native_core.hpp — C++ conveniences over the C ABI.
//
// This header adds NOTHING to the ABI. It is optional sugar for the C++ side
// of the boundary: the canonical contract is native_core.h. Everything here is
// header-only, constexpr/inline, and must never appear in a struct that crosses
// the FFI.
#pragma once

#include <pulp/native_components/native_core.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace pulp::native_components {

inline constexpr std::uint32_t kAbiVersion = PULP_NATIVE_CORE_ABI_VERSION;

// FNV-1a (64-bit) over UTF-8 id bytes — the SINGLE definition of parameter
// identity hashing for both the host and any binding generator. Case-sensitive,
// computed over the exact byte sequence. Must match the algorithm documented in
// native_core.h (offset basis 0xcbf29ce484222325, prime 0x100000001b3).
inline constexpr std::uint64_t param_id_hash(std::string_view id) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : id) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 0x100000001b3ULL;
    }
    return h;
}

// True when a vtable advertises a compatible ABI. Additive growth is tolerated:
// a newer host accepts an older, smaller `size` as long as the major
// abi_version matches (decision 5).
inline bool is_compatible(const pulp_native_core_v1* core) noexcept {
    return core != nullptr && core->abi_version == kAbiVersion &&
           core->size >= offsetof(pulp_native_core_v1, descriptor);
}

// Every type that crosses the boundary must be a C-compatible POD: standard
// layout (stable field offsets) and trivially copyable (memcpy-safe). If any of
// these fire, the struct grew a non-POD member and the ABI is no longer C.
static_assert(std::is_standard_layout_v<pulp_native_descriptor_v1>);
static_assert(std::is_trivially_copyable_v<pulp_native_descriptor_v1>);
static_assert(std::is_standard_layout_v<pulp_native_param_v1>);
static_assert(std::is_trivially_copyable_v<pulp_native_param_v1>);
static_assert(std::is_standard_layout_v<pulp_native_param_event_v1>);
static_assert(std::is_trivially_copyable_v<pulp_native_param_event_v1>);
static_assert(std::is_standard_layout_v<pulp_native_param_event_view_v1>);
static_assert(std::is_standard_layout_v<pulp_native_audio_bus_v1>);
static_assert(std::is_standard_layout_v<pulp_native_audio_io_v1>);
static_assert(std::is_standard_layout_v<pulp_native_midi_event_v1>);
static_assert(std::is_standard_layout_v<pulp_native_midi_view_v1>);
static_assert(std::is_standard_layout_v<pulp_native_state_span_v1>);
static_assert(std::is_standard_layout_v<pulp_native_state_out_v1>);
static_assert(std::is_standard_layout_v<pulp_native_bus_layout_v1>);
static_assert(std::is_standard_layout_v<pulp_native_prepare_v1>);
static_assert(std::is_standard_layout_v<pulp_native_process_context_v1>);
static_assert(std::is_standard_layout_v<pulp_native_process_v1>);
static_assert(std::is_standard_layout_v<pulp_native_host_services_v1>);
static_assert(std::is_standard_layout_v<pulp_native_core_v1>);

}  // namespace pulp::native_components
