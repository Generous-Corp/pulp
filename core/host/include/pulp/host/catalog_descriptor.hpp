#pragma once

// Product-neutral catalog vocabulary.
//
// The first consumer of the descriptor machinery called it Forge, but the
// concepts are useful to any Pulp node provider. This header is an additive
// source-level seam over the existing implementation. Legacy Forge headers,
// symbols, JSON, and CLI output remain compatibility authorities; aliases here
// deliberately avoid an ABI or serialized-format migration.

#include <pulp/host/forge_catalog_export.hpp>
#include <pulp/host/forge_catalog_json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace pulp::host::catalog {

using ParameterKind = ForgeParamKind;
using ParameterCurve = ForgeParamCurve;
using ParameterChoice = ForgeParamChoice;
using ParameterDescriptor = ForgeParamDescriptor;
using AxisValue = ForgeAxisValue;
using RealizationAxis = ForgeRealizationAxis;
using RealizationSetting = ForgeRealizationSetting;
using Realization = ForgeRealization;
using Descriptor = ForgeNodeDescriptor;

using BakedParameter = CustomNodeBakedParam;
using AuditFault = ForgeAuditFault;
using Finding = ForgeAuditFinding;
using ExportRealization = ForgeCatalogExportRealization;
using ExportNode = ForgeCatalogExportNode;

inline std::vector<Finding> validate(const Descriptor& descriptor,
                                     const std::vector<BakedParameter>& baked,
                                     std::string_view realization_mode = {}) {
    return audit_forge_descriptor(descriptor, baked, realization_mode);
}

inline std::vector<Finding> validate_export(const std::vector<ExportNode>& nodes) {
    // Forge's fixed 89-node membership is product-specific. The neutral seam
    // validates only each provider's descriptor/factory join; Forge callers
    // retain audit_forge_catalog_export for the Forge inventory check.
    return audit_catalog_export(nodes);
}

inline std::string describe(const Finding& finding) {
    return describe_forge_audit_finding(finding);
}

// This is the compatibility projection for the existing Forge JSON schema.
// Neutral providers should serialize their own catalog schema rather than
// treating the `pulp.forge-catalog.v1` document as a generic contract.
inline std::string serialize(const std::vector<ExportNode>& nodes) {
    return serialize_forge_catalog_json(nodes);
}

} // namespace pulp::host::catalog
