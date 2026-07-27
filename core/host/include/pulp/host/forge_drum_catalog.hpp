#pragma once

// Forge bake-layer adapter for Pulp license-clean percussion engines.

#include <pulp/host/detail/forge_drum_catalog_impl.hpp>
#include <pulp/host/forge_drum_catalog_contract.hpp>

namespace pulp::host::forge_drum {

inline CustomNodeType make_drum_node(EngineId id) {
    return detail::make_drum_node_impl(id);
}

} // namespace pulp::host::forge_drum
