#pragma once

// Public semantic declarations for the dynamics catalog.
//
// The descriptor objects are assembled in forge_dynamics_catalog.cpp alongside
// their factories. Keeping only these declarations in this small header lets
// metadata consumers avoid pulling the family implementation into their own
// translation unit while preserving the historical dynamics namespace API.

#include <pulp/host/forge_param_descriptor.hpp>

namespace pulp::host::dynamics {

ForgeNodeDescriptor feedforward_compressor_descriptor();

namespace true_peak {
ForgeNodeDescriptor descriptor();
}  // namespace true_peak

namespace vca {
ForgeNodeDescriptor vca_compressor_descriptor();
}  // namespace vca

namespace fet {
ForgeNodeDescriptor fet_compressor_descriptor();
}  // namespace fet

namespace diode {
ForgeNodeDescriptor diode_bridge_compressor_descriptor();
}  // namespace diode

}  // namespace pulp::host::dynamics
