#pragma once

/// @file catalogue.hpp
/// Stable-id construction of the retained Pulp view components as Vellum views.
///
/// A host or script names a component by id and receives a
/// `vellum::view::View`; the component's own type stays reachable by
/// `dynamic_cast` for callers that need its API. The context carries the
/// Pulp-owned services a component binds to, so Vellum headers never name a
/// Pulp state type.

#include <pulp/state/properties_file.hpp>
#include <pulp/vellum_components/aliases.hpp>
#include <pulp/vellum_components/panic_button.hpp>

#include <memory>
#include <string_view>
#include <vector>

namespace pulp::vellum_components {

/// Services the catalogue's components bind to. Every member is optional; a
/// component whose service is absent is still constructed and starts inert.
struct PulpViewComponentContext {
    /// Where the panic button raises its request. Empty means unavailable.
    PanicTarget panic;
    /// Persistence for the preferences panel's active page. Not owned; must
    /// outlive the panel.
    state::PropertiesFile* preferences = nullptr;
};

/// Ids accepted by `make_pulp_view_component`, in catalogue order.
const std::vector<std::string_view>& pulp_view_component_ids();

/// Construct the component registered under `component_id`, or nullptr when
/// the id is unknown.
std::unique_ptr<::vellum::view::View>
make_pulp_view_component(std::string_view component_id, const PulpViewComponentContext& context);

} // namespace pulp::vellum_components
