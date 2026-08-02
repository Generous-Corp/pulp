#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline/transaction.hpp>

#include <cstddef>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pulp::timeline_agent_view {

inline constexpr std::uint32_t kAgentViewVersion = 1;

enum class ErrorCode : std::uint8_t {
    InvalidSnapshot,
    StaleRevision,
    MissingSequence,
    InvalidRange,
    InvalidCursor,
    InvalidProvenance,
    InvalidDirtySet,
    LimitExceeded,
    SerializationFailed,
};

struct Error {
    ErrorCode code = ErrorCode::InvalidSnapshot;
    timeline::DocumentRevision expected_revision;
    timeline::DocumentRevision actual_revision;
    timeline::ItemId item;
};

struct Limits {
    std::size_t max_outline_items = 100'000;
    std::size_t max_census_items = 1'000'000;
    std::size_t max_page_items = 1'000;
    std::size_t max_canonical_bytes = 1024ull * 1024ull * 1024ull;
};

/// Count and SHA-256 commitment for authored rows omitted directly at one node.
/// Child outline rows and their omissions are excluded, so all node counts form
/// one non-overlapping partition of the non-outline ProjectSnapshotCounts rows.
struct OmissionSummary {
    std::size_t count = 0;
    std::string sha256;
};

struct ClipSummary {
    timeline::ItemId sequence_id;
    timeline::ItemId track_id;
    timeline::ItemId id;
    timeline::ClipTimeAnchor anchor = timeline::ClipTimeAnchor::Musical;
    std::int64_t start = 0;
    std::uint64_t duration = 0;
    /// Merkle commitment to the complete clip row, including its omission hash.
    std::string content_sha256;
    OmissionSummary omitted;
};

struct TrackSummary {
    timeline::ItemId sequence_id;
    timeline::ItemId id;
    std::string name;
    std::string content_sha256;
    OmissionSummary omitted;
    std::vector<ClipSummary> clips;
};

struct SequenceSummary {
    timeline::ItemId id;
    std::string name;
    std::string content_sha256;
    OmissionSummary omitted;
    std::vector<TrackSummary> tracks;
};

struct Outline {
    std::uint32_t version = kAgentViewVersion;
    timeline::DocumentRevision revision;
    timeline::ItemId project_id;
    std::string project_name;
    std::string content_sha256;
    timeline::ProjectSnapshotCounts census;
    std::size_t explicit_item_count = 0;
    OmissionSummary omitted;
    std::vector<SequenceSummary> sequences;
};

struct RegionCursor {
    std::uint32_t version = kAgentViewVersion;
    timeline::DocumentRevision revision;
    timeline::ItemId sequence_id;
    timeline::ClipTimeAnchor anchor = timeline::ClipTimeAnchor::Musical;
    std::int64_t window_start = 0;
    std::int64_t window_end = 0;
    std::int64_t start = 0;
    timeline::ItemId clip_id;
    constexpr auto operator<=>(const RegionCursor&) const = default;
};

struct RegionRequest {
    timeline::DocumentRevision expected_revision;
    timeline::ItemId sequence_id;
    timeline::ClipTimeAnchor anchor = timeline::ClipTimeAnchor::Musical;
    std::int64_t start = 0;
    /// Half-open start-position window [start, end).
    std::int64_t end = 0;
    std::size_t limit = 100;
    std::optional<RegionCursor> after;
};

struct RegionPage {
    std::uint32_t version = kAgentViewVersion;
    timeline::DocumentRevision revision;
    std::vector<ClipSummary> items;
    std::optional<RegionCursor> next;
};

enum class OutlineKind : std::uint8_t { Project, Sequence, Track, Clip };

struct OutlineChange {
    OutlineKind kind = OutlineKind::Project;
    timeline::ItemId sequence_id;
    timeline::ItemId track_id;
    timeline::ItemId item_id;
    timeline::DirtyFlags flags = timeline::DirtyFlags::None;
    constexpr auto operator<=>(const OutlineChange&) const = default;
};

struct OutlineDiff {
    std::uint32_t version = kAgentViewVersion;
    timeline::DocumentRevision revision;
    std::vector<OutlineChange> changes;
};

/// Adjacent revision range associated by the caller with one DirtySet.
///
/// Adjacency rejects stale and multi-commit ranges, but the public DirtySet type
/// carries no session-issued provenance token, so this value cannot authenticate
/// which commit produced the supplied set.
struct DirtyRevisionRange {
    timeline::DocumentRevision before;
    timeline::DocumentRevision after;
};

/// Immutable, bounded projection of one pinned DocumentView.
class AgentView {
  public:
    static runtime::Result<AgentView, Error>
    create(timeline::DocumentView view, Limits limits = {});

    timeline::DocumentRevision revision() const noexcept { return view_.revision; }

    runtime::Result<Outline, Error>
    outline(timeline::DocumentRevision expected_revision) const;
    runtime::Result<RegionPage, Error> region(const RegionRequest& request) const;
    /// Projects a DirtySet across one adjacent transition ending at this pin.
    ///
    /// Callers obtain freshness from their live session because a pin cannot
    /// discover a later commit by itself. Adjacency is validated, but DirtySet
    /// has no origin token, so callers remain responsible for pairing the set
    /// with the CommitResult that produced it.
    runtime::Result<OutlineDiff, Error>
    diff(timeline::DocumentRevision expected_revision, DirtyRevisionRange revisions,
         const timeline::DirtySet& dirty) const;

  private:
    AgentView(timeline::DocumentView view, Limits limits,
              timeline::ProjectSnapshotCounts counts)
        : view_(std::move(view)), limits_(limits), counts_(counts) {}

    timeline::DocumentView view_;
    Limits limits_;
    timeline::ProjectSnapshotCounts counts_;
};

} // namespace pulp::timeline_agent_view
