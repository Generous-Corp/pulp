#pragma once

#include <pulp/format/standalone_control_host.hpp>

#include <memory>

namespace pulp::inspect {

/// Creates the canonical broker-enrolled host bridge for an explicitly
/// control-enabled Standalone executable. A direct launch remains inert; only
/// the broker's inherited, kernel-authenticated bootstrap can open the host.
std::unique_ptr<format::StandaloneControlHost> make_control_standalone_host();

} // namespace pulp::inspect
