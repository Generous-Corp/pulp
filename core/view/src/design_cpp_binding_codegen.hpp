#pragma once

#include <pulp/view/design_codegen.hpp>

#include "design_import_native_common.hpp"

#include <string>

namespace pulp::view {

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
