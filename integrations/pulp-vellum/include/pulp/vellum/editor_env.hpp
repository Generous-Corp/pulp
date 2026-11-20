#pragma once

/// @file
/// `pulp::vellum::EditorEnv`: what a plug-in's root Vellum view receives.
///
/// Kept apart from editor.hpp so a root view, and a headless test or capture
/// of it, compiles without Vellum's plug-in view host.

#include <pulp/format/editor_host.hpp>
#include <pulp/vellum/param_port_model.hpp>

#include <vellum/value/value_feed.hpp>

namespace pulp::vellum {

struct EditorEnv {
    format::EditorContext& context;
    ParamPortModel& params;
    ::vellum::value::ValueFeed* visuals = nullptr;
};

} // namespace pulp::vellum
