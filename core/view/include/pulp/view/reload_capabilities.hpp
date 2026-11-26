#pragma once
// The capability vocabulary is defined in runtime, so the format layer can
// validate a pack's declared capabilities without the view layer. The bridge
// enforces it here under the view names.
#include <pulp/runtime/reload_capabilities.hpp>

namespace pulp::view {

using runtime::capability_from_name;
using runtime::capability_name;
using runtime::CapabilitySet;
using runtime::ReloadCapability;

} // namespace pulp::view
