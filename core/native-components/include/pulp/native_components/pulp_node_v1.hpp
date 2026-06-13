// pulp/native_components/pulp_node_v1.hpp — C++ conveniences over the public
// node ABI. Adds NOTHING to the ABI; the contract is pulp_node_v1.h.
#pragma once

#include <pulp/native_components/pulp_node_v1.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pulp::native_components {

inline constexpr std::uint32_t kNodeAbiMajor = PULP_NODE_V1_ABI_MAJOR;
inline constexpr std::size_t kNodeEntryMinimumSize =
    offsetof(pulp_node_entry_v1, descriptor)
    + sizeof(((pulp_node_entry_v1*)nullptr)->descriptor);

// The public node ABI major tracks the runtime node-ABI generation
// (pulp::PULP_NODE_ABI_VERSION in <pulp/runtime/node_abi.hpp>) so a single
// number identifies the node interface across the source API (CustomNodeType)
// and this frozen binary ABI. That cross-module equality is asserted in the
// test (test_pulp_node_v1.cpp) rather than here, so this header — and the
// pulp::native-components module — stays dependency-free.

// True when an entry advertises a compatible ABI. Same-major + at least the
// minimum size (through the first required callback) is accepted; trailing
// fields a newer node adds are ignored by an older host.
inline bool node_is_compatible(const pulp_node_entry_v1* entry) noexcept {
    return entry != nullptr && entry->abi_major == kNodeAbiMajor &&
           entry->size >= kNodeEntryMinimumSize;
}

// Every boundary struct must be a C-compatible POD.
static_assert(std::is_standard_layout_v<pulp_node_descriptor_v1>);
static_assert(std::is_trivially_copyable_v<pulp_node_descriptor_v1>);
static_assert(std::is_standard_layout_v<pulp_node_audio_v1>);
static_assert(std::is_standard_layout_v<pulp_node_prepare_v1>);
static_assert(std::is_standard_layout_v<pulp_node_host_services_v1>);
static_assert(std::is_standard_layout_v<pulp_node_writer_v1>);
static_assert(std::is_standard_layout_v<pulp_node_entry_v1>);
static_assert(std::is_trivially_copyable_v<pulp_node_entry_v1>);

}  // namespace pulp::native_components
