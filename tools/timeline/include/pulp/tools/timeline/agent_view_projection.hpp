#pragma once

#include <pulp/timeline_agent_view/agent_view.hpp>
#include <pulp/tools/timeline/agent.hpp>
#include <pulp/tools/timeline/writer_profile.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// @file
/// Offline JSON projection of the versioned AgentView read surface.
///
/// AgentView returns C++ values; every offline consumer — the `pulp seq view`
/// verbs, the MCP tools, and any UI reading through either — needs the same
/// bytes. Encoding it once here keeps the CLI and MCP payloads identical by
/// construction rather than by review, and gives a consumer a single shape to
/// pin against.
///
/// Every payload carries the `version` field the projection was produced at, so
/// a consumer can refuse a projection it does not understand instead of
/// misreading one. Identities and revisions are emitted as decimal *strings*
/// because they are 64-bit and a JSON number is not required to survive one;
/// an unset identity is `null` rather than `"0"`.
namespace pulp::tools::timeline {

/// Bounded window request for `view_region`, in the caller's own vocabulary.
///
/// This is the transport form of `timeline_agent_view::RegionRequest`: the
/// cursor arrives as its encoded string rather than as a parsed struct, because
/// a caller round-trips the token it was handed without inspecting it.
struct RegionViewOptions {
    /// Sequence to page through. Required.
    std::uint64_t sequence_id = 0;
    /// Window start, in the units named by `absolute`.
    std::int64_t start = 0;
    /// Window end, exclusive.
    std::int64_t end = 0;
    /// Page size. Clamped by AgentView's own limits.
    std::size_t limit = 100;
    /// Selects the absolute timebase rather than the musical one.
    bool absolute = false;
    /// Continuation token from a prior page's `next`, verbatim.
    std::string after;
};

/// Encodes an outline payload as canonical UTF-8 JSON with sorted keys.
std::string agent_view_outline_json(const pulp::timeline_agent_view::Outline& outline);

/// Encodes one bounded region page, including its continuation token.
std::string agent_view_region_page_json(const pulp::timeline_agent_view::RegionPage& page);

/// Encodes an outline diff over a revision range.
std::string agent_view_outline_diff_json(const pulp::timeline_agent_view::OutlineDiff& diff);

/// Encodes an AgentView refusal in the offline error envelope.
///
/// The stable `code` name is what a consumer branches on; `code_id` is the
/// numeric form for a consumer that cannot carry the name.
std::string agent_view_error_json(std::string_view stage,
                                  const pulp::timeline_agent_view::Error& error);

/// Returns the stable lowercase name for an AgentView refusal code.
std::string_view agent_view_error_code_name(pulp::timeline_agent_view::ErrorCode code) noexcept;

/// Encodes a continuation cursor as the opaque token a caller hands back.
std::string region_cursor_token(const pulp::timeline_agent_view::RegionCursor& cursor);

/// Parses a continuation token produced by `region_cursor_token`.
///
/// Returns `std::nullopt` when the token is malformed. A token that parses but
/// names a different revision or window is rejected later by AgentView itself,
/// which owns that judgement.
std::optional<pulp::timeline_agent_view::RegionCursor>
parse_region_cursor_token(std::string_view token);

/// Projects a whole project as a bounded outline.
OperationResult view_outline(const ProjectSource& project);

/// Projects one bounded window of clips from a single sequence.
OperationResult view_region(const ProjectSource& project, const RegionViewOptions& options);

/// Applies commands to an in-memory copy of the project and projects the
/// resulting outline diff.
///
/// A diff needs two revisions and the dirty set between them, neither of which
/// exists for a project that was only opened. Producing them from an applied
/// transaction is what makes the verb answerable offline; the project on disk
/// is never written.
OperationResult view_diff(const ProjectSource& project, std::string_view commands,
                          const WriterProfile& profile);

} // namespace pulp::tools::timeline
