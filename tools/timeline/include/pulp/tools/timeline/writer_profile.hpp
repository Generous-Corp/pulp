#pragma once

#include <pulp/timeline/command.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/transaction.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::tools::timeline {

/** @addtogroup tools_timeline
 * @{
 */

/// Named authority an offline boundary admits a document writer under.
///
/// The offline surfaces register a writer on the caller's behalf, so the
/// authority that writer holds is a property of the boundary rather than of the
/// caller. Naming it makes that choice selectable and discoverable instead of
/// implicit.
enum class WriterProfileKind : std::uint8_t {
    /// Every command class, no destructive intent, finite quotas.
    ///
    /// A caller may add and modify content but cannot remove any. Destructive
    /// intent is refused as `capability_denied` rather than silently dropped.
    Proposal,
    /// Every command class and intent, under finite quotas.
    Editor,
    /// Every command class and intent, with no quota.
    ///
    /// Never selected on a caller's behalf: a boundary must not default to it.
    Trusted,
};

/// A writer authority paired with the identity it is published under.
struct WriterProfile {
    WriterProfileKind kind = WriterProfileKind::Proposal;
    pulp::timeline::WriterCapabilityMask mask;
};

/// Retained-byte ceilings for the quota-bounded profiles.
///
/// Finite by construction. `std::numeric_limits<std::size_t>::max()` is
/// reserved for `trusted`, so a bounded profile can never be widened into an
/// unbounded one by adjusting a constant here.
inline constexpr std::size_t kProposalMaxTransactionRetainedBytes = 1024ull * 1024ull;
inline constexpr std::size_t kProposalMaxSessionRetainedBytes = 32ull * 1024ull * 1024ull;
inline constexpr std::size_t kEditorMaxTransactionRetainedBytes = 16ull * 1024ull * 1024ull;
inline constexpr std::size_t kEditorMaxSessionRetainedBytes = 256ull * 1024ull * 1024ull;

/// Returns the non-destructive, quota-bounded proposal authority.
WriterProfile proposal_writer_profile() noexcept;
/// Returns the fully capable, quota-bounded editor authority.
WriterProfile editor_writer_profile() noexcept;
/// Returns the fully capable, unquotaed trusted authority.
WriterProfile trusted_writer_profile() noexcept;

/// Resolves a profile from its stable public name.
///
/// Returns no value for an unrecognized name so a boundary fails closed with a
/// usage error rather than falling back to a more permissive authority.
std::optional<WriterProfile> writer_profile_by_name(std::string_view name) noexcept;

/// Returns the stable public name of `kind`, part of the CLI and MCP contract.
std::string_view writer_profile_name(WriterProfileKind kind) noexcept;

/// Returns the selectable profile names, in the order help text lists them.
std::string_view selectable_writer_profile_names() noexcept;

/// Returns the stable public name of a command class.
///
/// Capability crosses a boundary by class and intent name only. `capability_bit`
/// is `class_index * kCommandIntentCount + intent`, so inserting a CommandClass
/// renumbers every later bit; a bit index is not a durable external identity.
std::string_view command_class_name(pulp::timeline::CommandClass value) noexcept;
/// Returns the stable public name of a command intent.
std::string_view command_intent_name(pulp::timeline::CommandIntent value) noexcept;
/// Returns the stable public name of a transaction conflict.
std::string_view conflict_code_name(pulp::timeline::ConflictCode code) noexcept;

/// Serializes `mask` as named class/intent pairs and quota ceilings.
///
/// Emits names rather than the raw `allowed` integer, and emits a null quota
/// for an unbounded ceiling rather than a value beyond exact JSON number range.
std::string writer_capability_json(const pulp::timeline::WriterCapabilityMask& mask);

/// Serializes the full discoverable capability surface of `profile`.
std::string writer_profile_json(const WriterProfile& profile);

/// One command's identity paired with the authority it required.
struct CommandAuthorityRecord {
    pulp::timeline::CommandId command;
    pulp::timeline::CommandAuthority authority;
};

/// Captures the authority every command in `transaction` declares.
///
/// Taken before submission because a rejected transaction is not guaranteed to
/// remain readable, and a refusal must still be able to name the denied axis.
///
/// This is the authority the command itself declares. A command that removes a
/// container also requires its children's classes, so for those the declared
/// authority names the command's own axis and is not the exhaustive set the
/// session admitted against.
std::vector<CommandAuthorityRecord>
capture_command_authorities(const pulp::timeline::Transaction& transaction);

/// Serializes a rejected transaction as a typed refusal envelope.
///
/// Names the conflict, the offending command, and, when the refusal is a
/// capability denial, the class and intent the command required. `stage` names
/// the boundary operation that refused.
std::string transaction_refusal_json(const pulp::timeline::TransactionError& error,
                                     std::string_view stage,
                                     const std::vector<CommandAuthorityRecord>& authorities);

/// @}

} // namespace pulp::tools::timeline
