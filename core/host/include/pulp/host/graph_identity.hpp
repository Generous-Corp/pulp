#pragma once

// Control-side identity helpers for authored graph ports.  These helpers are
// value-only: they never inspect a live snapshot and are safe to retain after
// a graph mutation.  Runtime scheduling and canonical port numbers are not
// part of this identity surface.

#include <pulp/host/signal_graph_node.hpp>
#include <pulp/host/prepared_description.hpp>
#include <pulp/runtime/result.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pulp::host {

struct PortIdentityAlias {
    // The stable authored spelling and the replacement spelling supplied by
    // the source contract.  Resolution is deliberately one hop.
    std::string authored_name;
    std::string replacement_name;
    std::uint32_t contract_version = 0;
};

struct PortIdentityIssue {
    enum class Code : std::uint8_t {
        InvalidEncoding,
        EmptyName,
        ControlCharacter,
        DuplicateName,
        InvalidAlias,
        AliasChain,
        AliasCollision,
    };
    Code code = Code::InvalidEncoding;
    std::string name;
    std::string detail;
};

struct PortIdentityReport {
    bool valid = false;
    std::vector<std::string> normalized_names;
    std::vector<PortIdentityIssue> issues;
};

// Validate and NFC-normalize a port set according to docs/contracts/
// g3-identity-contract.md.  The result is deterministic and owns all strings.
PortIdentityReport validate_port_identities(std::span<const PortHandle> ports,
                                            std::span<const PortIdentityAlias> aliases = {});

// Stable semantic fingerprint of a validated identity set.  Returns zero when
// validation fails.  Canonical port indexes and insertion order are excluded.
std::uint64_t port_identity_fingerprint(std::span<const PortHandle> ports,
                                        std::span<const PortIdentityAlias> aliases = {});

struct PortIdentityDiff {
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> renamed;
    bool order_changed = false;
    bool valid = false;
};

PortIdentityDiff diff_port_identities(std::span<const PortHandle> before,
                                      std::span<const PortHandle> after,
                                      std::span<const PortIdentityAlias> aliases = {});

// A compact value-owned undo record.  Applying undo is a caller-owned graph
// mutation; this type intentionally has no access to SignalGraph internals.
struct PortIdentityUndo {
    std::vector<PortHandle> before;
    bool valid = false;
};

PortIdentityUndo capture_port_identity_undo(std::span<const PortHandle> before,
                                            std::span<const PortIdentityAlias> aliases = {});

std::vector<PortHandle> undo_port_identity(const PortIdentityUndo& undo);

enum class GraphIdentityError : std::uint8_t {
    UnsupportedPreparedProfile,
    ReceiptBindingInvalid,
    DigestPayloadMismatch,
    IncompatibleProfile,
    AllocationFailed,
};

// Only validated constructors below can mint an identity value.
class GraphIdentityValue {
  public:
    std::string_view domain() const noexcept { return domain_; }
    std::string_view profile() const noexcept { return profile_; }
    std::string_view canonical_bytes() const noexcept { return payload_; }
    std::string_view digest() const noexcept { return digest_; }
  private:
    std::string domain_, profile_, payload_, digest_;
    friend struct ResolvedIdentityAccess;
    friend struct GraphIdentityReceipt;
    friend runtime::Result<bool, GraphIdentityError> compare_graph_identities(
        const GraphIdentityValue&, const GraphIdentityValue&);
};

using GraphIdentityResult = runtime::Result<GraphIdentityValue, GraphIdentityError>;

// Detached historical receipt; provenance is a claim, never live authority.
struct GraphIdentityReceipt {
    GraphIdentityValue authored;
    GraphIdentityValue resolved;
    std::string capture_provenance;
    std::string artifact_provenance;
    std::string canonical_bytes() const;
};
using GraphReceiptResult = runtime::Result<GraphIdentityReceipt, GraphIdentityError>;

GraphIdentityResult resolved_fingerprint(const PreparedDescriptionData&);
GraphReceiptResult capture_graph_identity_receipt(const PreparedDescriptionData&);
GraphReceiptResult decode_graph_identity_receipt(std::span<const std::byte>);
runtime::Result<bool, GraphIdentityError> compare_graph_identities(
    const GraphIdentityValue&, const GraphIdentityValue&);
const char* graph_identity_error_name(GraphIdentityError) noexcept;

} // namespace pulp::host
