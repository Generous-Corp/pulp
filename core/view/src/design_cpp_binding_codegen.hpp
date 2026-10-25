#pragma once

#include <pulp/view/design_codegen.hpp>

#include "design_import_native_common.hpp"

#include "design_binding_metadata.hpp"

#include <optional>
#include <string>

namespace pulp::view {

/// Whether an imported control's binding metadata is complete enough for a
/// binding to be installed: a route id, a stable anchor, and the keys its
/// widget kind reads (a parameter key for a scalar control, both axes for an
/// XY pad, a source and channel for a meter, a value key for a text field, a
/// host action for a button). The native binder, the baked C++ helpers and the
/// SwiftUI exporter bind exactly the controls this accepts, so one import binds
/// the same set in every lane.
bool binding_route_installable(const IRNode& node, NativeWidgetKind kind,
                               const NativeBindingMetadata& md);

/// Why a control that carries binding metadata gets no binding, in the native
/// binder's own diagnostic code and wording. Empty when the control binds, or
/// carries nothing to bind, which the native binder also passes over silently.
struct BindingRefusal {
    std::string code;
    std::string message;
    std::optional<std::string> property;
};
std::optional<BindingRefusal> binding_refusal(const IRNode& node, NativeWidgetKind kind,
                                              const NativeBindingMetadata& md);

struct BindingCodegenArtifacts {
    std::string binding_manifest;
    std::string helper_source;
    bool has_helpers = false;
};

BindingCodegenArtifacts generate_cpp_binding_artifacts(
    const IRNode& root,
    const ResolvedNativeNode& resolved,
    const CppExportOptions& opts);

}  // namespace pulp::view
