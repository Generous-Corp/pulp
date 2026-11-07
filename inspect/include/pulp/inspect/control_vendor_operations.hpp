#pragma once

#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/control_manifest.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::inspect {

/// Schema identifier of the vendor operation declaration a controlled target
/// embeds in its control manifest.
inline constexpr std::string_view kControlVendorOperationsSchemaId =
    "dev.pulp.control/vendor-operations@1";
inline constexpr std::size_t kControlVendorOperationsMaximumBytes = 128u * 1024u;
inline constexpr std::size_t kControlVendorOperationsMaximumCount = 64;
inline constexpr std::size_t kControlVendorIdMaximumBytes = 96;
inline constexpr std::size_t kControlVendorOperationIdMaximumBytes = 192;
inline constexpr std::size_t kControlVendorDescriptionMaximumBytes = 512;

/// The policy tier a vendor operation declares. Each tier is carried by one
/// fixed Pulp capability, so a vendor operation is granted, consented, leased,
/// and receipted exactly like the Pulp operations of the same risk.
enum class ControlVendorTier : std::uint8_t {
    Read,        ///< dev.pulp.vendor/read@1: no side effects; observe profile.
    Gesture,     ///< dev.pulp.vendor/gesture@1: reversible edit; develop profile, controller lease.
    Render,      ///< dev.pulp.vendor/render@1: resource-consuming; single-operation grant only.
    Destructive, ///< dev.pulp.vendor/destructive@1: irreversible; single-operation grant and lease.
};

std::string_view control_vendor_tier_id(ControlVendorTier tier);
std::optional<ControlVendorTier> control_vendor_tier_from_id(std::string_view id);
InspectorCapability control_vendor_tier_capability(ControlVendorTier tier);
std::optional<ControlVendorTier> control_vendor_tier_for_capability(InspectorCapability capability);
bool capability_is_vendor_tier(InspectorCapability capability);

/// A vendor id is a lowercase reverse-DNS name of two to eight labels, such as
/// `com.acme` or `com.acme.synth`. Names whose first label is `pulp`, and the
/// `dev.pulp` subtree, are reserved for Pulp itself.
bool control_vendor_id_is_valid(std::string_view vendor_id);

struct ControlVendorOperationIdParts {
    std::string_view vendor_id;
    std::string_view name;
    std::uint32_t version = 0;
};

/// Splits `dev.<vendor>/<name>@<version>`. `<name>` is one to six lowercase
/// segments separated by `/` or `.`; `<version>` is a positive decimal without
/// a leading zero. Returns nothing for any `dev.pulp.` operation or for an id
/// that does not follow the grammar.
std::optional<ControlVendorOperationIdParts>
parse_control_vendor_operation_id(std::string_view operation_id);

/// True when `operation_id` is syntactically a vendor operation id. This never
/// implies that any live instance declares it.
bool is_control_vendor_operation_id(std::string_view operation_id);

struct ControlVendorOperation {
    std::string id;
    std::uint32_t version = 1;
    ControlVendorTier tier = ControlVendorTier::Read;
    std::string description;
    /// Compact JSON re-serialization of the declared schema.
    std::string input_schema_json;
    std::string output_schema_json;
};

enum class ControlVendorOperationsError : std::uint8_t {
    None,
    Encoding,
    Limit,
    Parse,
    UnknownField,
    UnsupportedSchema,
    InvalidVendor,
    VendorBundleMismatch,
    InvalidOperation,
    DuplicateOperation,
    InvalidTier,
    InvalidDescription,
    InvalidSchema,
};

std::string_view control_vendor_operations_error_id(ControlVendorOperationsError error);

struct ControlVendorOperationsDiagnostics {
    ControlVendorOperationsError code = ControlVendorOperationsError::None;
    std::string error;
};

/// Immutable, validated vendor operation table for one control manifest.
/// Descriptor views point into this table, so callers keep the shared owner
/// alive for as long as they use a returned descriptor.
class ControlVendorOperationTable {
  public:
    ControlVendorOperationTable(std::string vendor_id, std::vector<ControlVendorOperation> operations);
    ControlVendorOperationTable(const ControlVendorOperationTable&) = delete;
    ControlVendorOperationTable& operator=(const ControlVendorOperationTable&) = delete;

    const std::string& vendor_id() const {
        return vendor_id_;
    }
    std::span<const ControlVendorOperation> operations() const {
        return operations_;
    }
    const ControlVendorOperation* find(std::string_view id, std::uint32_t version) const;
    /// The broker-facing contract of one declared operation. Its capability is
    /// the tier capability; it never produces artifacts and carries no
    /// operation-specific receipt field (the durable receipt still exists).
    const ControlOperationDescriptor* descriptor(std::string_view id,
                                                 std::uint32_t version) const;
    bool declares_tier(ControlVendorTier tier) const;

  private:
    std::string vendor_id_;
    std::vector<ControlVendorOperation> operations_;
    std::vector<std::pair<std::string, std::string>> schema_ids_;
    std::vector<ControlOperationDescriptor> descriptors_;
};

/// Parses and validates a vendor operation declaration. The document must be
/// printable ASCII (plus tab, carriage return and newline) so its exact bytes
/// survive the manifest's JSON string encoding identically in CMake and C++.
/// The vendor id must equal `bundle_id` or be one of its reverse-DNS prefixes.
/// Every schema must be a closed, bounded object schema (see
/// `validate_control_vendor_schema`).
std::shared_ptr<const ControlVendorOperationTable>
parse_control_vendor_operations(std::string_view document, std::string_view bundle_id,
                                ControlVendorOperationsDiagnostics* diagnostics = nullptr);

/// Checks that an author-supplied schema stays inside the closed subset the
/// broker validator enforces: every object closes `additionalProperties`,
/// every string has `maxLength`, every array has `items` and `maxItems`, and
/// no keyword outside the subset appears. `root` additionally requires an
/// object type at the top level.
bool validate_control_vendor_schema(std::string_view schema_json, std::string& error,
                                    bool root = true);

} // namespace pulp::inspect
