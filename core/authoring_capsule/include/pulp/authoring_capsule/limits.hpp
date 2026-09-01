#pragma once

/// @file limits.hpp
/// Archive and path budgets. Every one of these fails closed.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pulp::authoring_capsule {

/// Fixed path of the manifest, which must also be the archive's first member.
/// It lives beside the budgets because it is a container fact: the reader
/// requires it and the writer holds itself to the same rule, so neither may
/// learn it from the envelope layer above.
inline constexpr std::string_view kManifestPath = "capsule.json";

struct CapsuleLimits {
    std::size_t max_members = 20000;
    std::uint64_t max_compressed_bytes = 2ull * 1024 * 1024 * 1024;
    std::uint64_t max_expanded_bytes = 8ull * 1024 * 1024 * 1024;
    std::uint64_t max_member_expanded_bytes = 2ull * 1024 * 1024 * 1024;
    /// Expansion ratio ceiling, applied per member and to the archive total.
    std::uint32_t max_expansion_ratio = 200;
    std::uint64_t max_manifest_bytes = 16ull * 1024 * 1024;
    std::size_t max_path_bytes = 1024;
    std::size_t max_path_depth = 32;
    /// Working-set ceiling for admission. Admission is bounded memory: a
    /// capsule larger than this is still readable, member by member.
    std::uint64_t max_working_set_bytes = 256ull * 1024 * 1024;
};

/// The frozen v1 budgets. A consumer may tighten a field; loosening one is a
/// format change, not a configuration choice.
inline constexpr CapsuleLimits kCapsuleLimitsV1{};

}  // namespace pulp::authoring_capsule
