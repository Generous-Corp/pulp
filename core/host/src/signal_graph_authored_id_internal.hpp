// signal_graph_authored_id_internal.hpp: the per-graph salt and fresh-ID
// derivation behind GraphNode::authored_id.
//
// A fresh authored ID only has to differ between live graphs and between
// documents; it never has to be unpredictable. The salt is therefore drawn
// from an entropy device only where reading one is safe, and otherwise built
// from inputs every runtime has: the wall clock, the graph's address and its
// process-wide diagnostic identity. Under Emscripten the entropy device is
// crypto.getRandomValues, which AudioWorkletGlobalScope does not provide, so a
// WebAssembly AudioWorklet build always takes the entropy-free path.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace pulp::host::detail {

using AuthoredIdSalt = std::array<std::uint64_t, 2>;

/// Derive a salt from caller-supplied words. Pure: it reads no entropy device
/// and no clock. Changing any one input changes the salt.
AuthoredIdSalt authored_id_salt_from(std::uint64_t clock_word, std::uintptr_t graph_address,
                                     std::uint64_t graph_identity) noexcept;

/// The clock word for authored_id_salt_from. It uses system_clock, which
/// Emscripten maps to Date.now(); steady_clock maps to performance.now(),
/// which an AudioWorklet global scope does not provide.
std::uint64_t authored_id_clock_word() noexcept;

/// authored_id_salt_from(authored_id_clock_word(), address, identity): the
/// entropy-free salt, used under Emscripten and when no entropy device exists.
AuthoredIdSalt authored_id_salt_without_entropy(std::uintptr_t graph_address,
                                                std::uint64_t graph_identity) noexcept;

/// The fresh authored ID ("nid-" plus 32 hex digits) for a salt and the
/// graph's mint counter value.
std::string authored_id_from_salt(const AuthoredIdSalt& salt, std::uint64_t counter);

} // namespace pulp::host::detail
