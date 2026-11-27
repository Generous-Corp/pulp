#pragma once

/// @file catalogue.hpp
/// Stable-id construction of the retained Pulp view components as Vellum views.
///
/// A host or script names a component by id and receives a
/// `vellum::view::View`; the component's own type stays reachable by
/// `dynamic_cast` for callers that need its API. The context carries the
/// Pulp-owned services a component binds to, so Vellum headers never name a
/// Pulp state type.
///
/// This is not a `register_data_display` registration: data displays are
/// built from a value feed alone, while these components need a context of
/// Pulp-owned services, so the catalogue takes one at construction.

#include <pulp/vellum_components/aliases.hpp>
#include <pulp/vellum_components/panic_button.hpp>

#include <pulp/state/preset_library.hpp>

#include <memory>
#include <string_view>
#include <vector>

namespace pulp::vellum_components {

/// Services the catalogue's components bind to. Every member is optional; a
/// component whose service is absent is still constructed and starts inert,
/// except for components whose API cannot operate without that service. The
/// preset program editor is one such component and is not constructed until
/// `preset_library` is set.
struct PulpViewComponentContext {
    /// Where the panic button raises its request. Empty means unavailable.
    PanicTarget panic;
    /// The library whose MIDI program map the program editor edits. The editor
    /// holds a reference to it, so the catalogue returns no
    /// `pulp.preset_program_editor` while it is null.
    state::PresetLibrary* preset_library = nullptr;
};

/// Ids accepted by `make_pulp_view_component`, in catalogue order.
const std::vector<std::string_view>& pulp_view_component_ids();

/// Construct the component registered under `component_id`. Returns nullptr
/// when the id is unknown or a required service for that component is absent.
std::unique_ptr<::vellum::view::View>
make_pulp_view_component(std::string_view component_id, const PulpViewComponentContext& context);

} // namespace pulp::vellum_components
