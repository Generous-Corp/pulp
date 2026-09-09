#include <pulp/tools/timeline/agent_view_projection.hpp>

#include "timeline_agent_internal.hpp"

#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/serialize.hpp>

#include <array>
#include <charconv>
#include <string>
#include <utility>
#include <vector>

namespace pulp::tools::timeline {

namespace {

namespace av = pulp::timeline_agent_view;

using pulp::timeline::quote_json_string;

/// Separator for the flat continuation-token encoding below.
///
/// A colon cannot appear in any field: every field is a decimal integer, so the
/// split is unambiguous without escaping.
constexpr char kCursorSeparator = ':';

/// Emits a 64-bit identity as a quoted decimal string, or `null` when unset.
///
/// A JSON number is not required to carry 64 bits intact, and an unset identity
/// is genuinely absent rather than zero, so neither is emitted as a bare
/// number.
std::string id_json(pulp::timeline::ItemId id) {
    if (!id.valid())
        return "null";
    return quote_json_string(std::to_string(id.value));
}

/// Emits a revision as a quoted decimal string, for the same 64-bit reason.
std::string revision_json(pulp::timeline::DocumentRevision revision) {
    return quote_json_string(std::to_string(revision.value));
}

std::string_view anchor_name(pulp::timeline::ClipTimeAnchor anchor) noexcept {
    switch (anchor) {
    case pulp::timeline::ClipTimeAnchor::Musical:
        return "musical";
    case pulp::timeline::ClipTimeAnchor::Absolute:
        return "absolute";
    }
    return "musical";
}

std::string_view outline_kind_name(av::OutlineKind kind) noexcept {
    switch (kind) {
    case av::OutlineKind::Project:
        return "project";
    case av::OutlineKind::Sequence:
        return "sequence";
    case av::OutlineKind::Track:
        return "track";
    case av::OutlineKind::Clip:
        return "clip";
    }
    return "project";
}

std::string dirty_flags_json(pulp::timeline::DirtyFlags flags) {
    using pulp::timeline::DirtyFlags;
    constexpr std::pair<DirtyFlags, std::string_view> names[] = {
        {DirtyFlags::Structure, "structure"},   {DirtyFlags::Timing, "timing"},
        {DirtyFlags::Content, "content"},       {DirtyFlags::Notes, "notes"},
        {DirtyFlags::Added, "added"},           {DirtyFlags::Removed, "removed"},
        {DirtyFlags::Automation, "automation"}, {DirtyFlags::Take, "take"},
        {DirtyFlags::Freeze, "freeze"},         {DirtyFlags::Marker, "marker"},
        {DirtyFlags::Context, "context"},       {DirtyFlags::Mixer, "mixer"},
    };
    const auto bits = static_cast<std::uint16_t>(flags);
    std::string json = "[";
    bool first = true;
    for (const auto& [flag, name] : names) {
        if ((bits & static_cast<std::uint16_t>(flag)) == 0)
            continue;
        if (!first)
            json += ',';
        first = false;
        json += quote_json_string(name);
    }
    json += ']';
    return json;
}

std::string omission_json(const av::OmissionSummary& omitted) {
    return "{\"count\":" + std::to_string(omitted.count) +
           ",\"sha256\":" + quote_json_string(omitted.sha256) + "}";
}

std::string census_json(const pulp::timeline::ProjectSnapshotCounts& counts) {
    std::string json = "{";
    const std::pair<std::string_view, std::size_t> fields[] = {
        {"assets", counts.assets},
        {"automation_lanes", counts.automation_lanes},
        {"automation_points", counts.automation_points},
        {"chord_scale_events", counts.chord_scale_events},
        {"clips", counts.clips},
        {"device_placements", counts.device_placements},
        {"groove_steps", counts.groove_steps},
        {"macro_controls", counts.macro_controls},
        {"markers", counts.markers},
        {"midi_lane_points", counts.midi_lane_points},
        {"midi_lanes", counts.midi_lanes},
        {"modulation_routes", counts.modulation_routes},
        {"modulators", counts.modulators},
        {"notes", counts.notes},
        {"regions", counts.regions},
        {"scenes", counts.scenes},
        {"sequences", counts.sequences},
        {"slots", counts.slots},
        {"take_comp_segments", counts.take_comp_segments},
        {"take_lanes", counts.take_lanes},
        {"takes", counts.takes},
        {"tracks", counts.tracks},
    };
    bool first = true;
    for (const auto& [name, value] : fields) {
        if (!first)
            json += ',';
        first = false;
        json += quote_json_string(name);
        json += ':';
        json += std::to_string(value);
    }
    json += '}';
    return json;
}

std::string clip_json(const av::ClipSummary& clip) {
    return "{\"anchor\":" + std::string(quote_json_string(anchor_name(clip.anchor))) +
           ",\"content_sha256\":" + quote_json_string(clip.content_sha256) +
           ",\"duration\":" + quote_json_string(std::to_string(clip.duration)) +
           ",\"id\":" + id_json(clip.id) + ",\"omitted\":" + omission_json(clip.omitted) +
           ",\"sequence_id\":" + id_json(clip.sequence_id) +
           ",\"start\":" + quote_json_string(std::to_string(clip.start)) +
           ",\"track_id\":" + id_json(clip.track_id) + "}";
}

std::string clip_array_json(const std::vector<av::ClipSummary>& clips) {
    std::string json = "[";
    bool first = true;
    for (const auto& clip : clips) {
        if (!first)
            json += ',';
        first = false;
        json += clip_json(clip);
    }
    json += ']';
    return json;
}

std::string track_json(const av::TrackSummary& track) {
    std::string json = "{\"clips\":" + clip_array_json(track.clips) +
                       ",\"content_sha256\":" + quote_json_string(track.content_sha256) +
                       ",\"id\":" + id_json(track.id) +
                       ",\"name\":" + quote_json_string(track.name) +
                       ",\"omitted\":" + omission_json(track.omitted) +
                       ",\"sequence_id\":" + id_json(track.sequence_id) + "}";
    return json;
}

std::string sequence_json(const av::SequenceSummary& sequence) {
    std::string json = "{\"content_sha256\":" + quote_json_string(sequence.content_sha256) +
                       ",\"id\":" + id_json(sequence.id) +
                       ",\"name\":" + quote_json_string(sequence.name) +
                       ",\"omitted\":" + omission_json(sequence.omitted) + ",\"tracks\":[";
    bool first = true;
    for (const auto& track : sequence.tracks) {
        if (!first)
            json += ',';
        first = false;
        json += track_json(track);
    }
    json += "]}";
    return json;
}

/// Appends one decimal field and its separator to a continuation token.
void append_cursor_field(std::string& token, std::int64_t value, bool last) {
    token += std::to_string(value);
    if (!last)
        token += kCursorSeparator;
}

/// Parses one decimal field from a continuation token.
template <typename T> bool parse_field(std::string_view field, T& out) {
    if (field.empty())
        return false;
    const auto* begin = field.data();
    const auto* end = begin + field.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

} // namespace

using detail::failure;
using detail::persistence_message;

std::string_view agent_view_error_code_name(av::ErrorCode code) noexcept {
    switch (code) {
    case av::ErrorCode::InvalidSnapshot:
        return "invalid_snapshot";
    case av::ErrorCode::StaleRevision:
        return "stale_revision";
    case av::ErrorCode::MissingSequence:
        return "missing_sequence";
    case av::ErrorCode::InvalidRange:
        return "invalid_range";
    case av::ErrorCode::InvalidCursor:
        return "invalid_cursor";
    case av::ErrorCode::InvalidProvenance:
        return "invalid_provenance";
    case av::ErrorCode::InvalidDirtySet:
        return "invalid_dirty_set";
    case av::ErrorCode::LimitExceeded:
        return "limit_exceeded";
    case av::ErrorCode::SerializationFailed:
        return "serialization_failed";
    }
    return "invalid_snapshot";
}

std::string agent_view_error_json(std::string_view stage, const av::Error& error) {
    const auto name = agent_view_error_code_name(error.code);
    return "{\"error\":{\"actual_revision\":" + revision_json(error.actual_revision) +
           ",\"code\":" + std::string(quote_json_string(name)) +
           ",\"code_id\":" + std::to_string(static_cast<std::uint8_t>(error.code)) +
           ",\"expected_revision\":" + revision_json(error.expected_revision) +
           ",\"item_id\":" + id_json(error.item) + ",\"message\":" + quote_json_string(name) +
           ",\"stage\":" + quote_json_string(stage) + "},\"ok\":false}";
}

std::string agent_view_outline_json(const av::Outline& outline) {
    std::string json = "{\"census\":" + census_json(outline.census) +
                       ",\"content_sha256\":" + quote_json_string(outline.content_sha256) +
                       ",\"explicit_item_count\":" + std::to_string(outline.explicit_item_count) +
                       ",\"ok\":true,\"omitted\":" + omission_json(outline.omitted) +
                       ",\"project_id\":" + id_json(outline.project_id) +
                       ",\"project_name\":" + quote_json_string(outline.project_name) +
                       ",\"revision\":" + revision_json(outline.revision) + ",\"sequences\":[";
    bool first = true;
    for (const auto& sequence : outline.sequences) {
        if (!first)
            json += ',';
        first = false;
        json += sequence_json(sequence);
    }
    json += "],\"version\":" + std::to_string(outline.version) + "}";
    return json;
}

std::string agent_view_region_page_json(const av::RegionPage& page) {
    std::string json = "{\"items\":" + clip_array_json(page.items) + ",\"next\":";
    json += page.next ? quote_json_string(region_cursor_token(*page.next)) : "null";
    json += ",\"ok\":true,\"revision\":" + revision_json(page.revision) +
            ",\"version\":" + std::to_string(page.version) + "}";
    return json;
}

std::string agent_view_outline_diff_json(const av::OutlineDiff& diff) {
    std::string json = "{\"changes\":[";
    bool first = true;
    for (const auto& change : diff.changes) {
        if (!first)
            json += ',';
        first = false;
        json += "{\"flag_bits\":" + std::to_string(static_cast<std::uint16_t>(change.flags)) +
                ",\"flags\":" + dirty_flags_json(change.flags) +
                ",\"item_id\":" + id_json(change.item_id) +
                ",\"kind\":" + std::string(quote_json_string(outline_kind_name(change.kind))) +
                ",\"sequence_id\":" + id_json(change.sequence_id) +
                ",\"track_id\":" + id_json(change.track_id) + "}";
    }
    json += "],\"ok\":true,\"revision\":" + revision_json(diff.revision) +
            ",\"version\":" + std::to_string(diff.version) + "}";
    return json;
}

std::string region_cursor_token(const av::RegionCursor& cursor) {
    std::string token;
    append_cursor_field(token, static_cast<std::int64_t>(cursor.version), false);
    append_cursor_field(token, static_cast<std::int64_t>(cursor.revision.value), false);
    append_cursor_field(token, static_cast<std::int64_t>(cursor.sequence_id.value), false);
    append_cursor_field(token, static_cast<std::int64_t>(cursor.anchor), false);
    append_cursor_field(token, cursor.window_start, false);
    append_cursor_field(token, cursor.window_end, false);
    append_cursor_field(token, cursor.start, false);
    append_cursor_field(token, static_cast<std::int64_t>(cursor.clip_id.value), true);
    return token;
}

std::optional<av::RegionCursor> parse_region_cursor_token(std::string_view token) {
    std::array<std::string_view, 8> fields{};
    std::size_t count = 0;
    std::size_t begin = 0;
    while (begin <= token.size()) {
        const auto found = token.find(kCursorSeparator, begin);
        const auto end = found == std::string_view::npos ? token.size() : found;
        if (count == fields.size())
            return std::nullopt;
        fields[count++] = token.substr(begin, end - begin);
        if (found == std::string_view::npos)
            break;
        begin = end + 1;
    }
    if (count != fields.size())
        return std::nullopt;

    std::uint32_t version = 0;
    std::uint64_t revision = 0;
    std::uint64_t sequence_id = 0;
    std::uint8_t anchor = 0;
    std::int64_t window_start = 0;
    std::int64_t window_end = 0;
    std::int64_t start = 0;
    std::uint64_t clip_id = 0;
    if (!parse_field(fields[0], version) || !parse_field(fields[1], revision) ||
        !parse_field(fields[2], sequence_id) || !parse_field(fields[3], anchor) ||
        !parse_field(fields[4], window_start) || !parse_field(fields[5], window_end) ||
        !parse_field(fields[6], start) || !parse_field(fields[7], clip_id))
        return std::nullopt;
    if (anchor > static_cast<std::uint8_t>(pulp::timeline::ClipTimeAnchor::Absolute))
        return std::nullopt;

    av::RegionCursor cursor;
    cursor.version = version;
    cursor.revision = pulp::timeline::DocumentRevision{revision};
    cursor.sequence_id = pulp::timeline::ItemId{sequence_id};
    cursor.anchor = static_cast<pulp::timeline::ClipTimeAnchor>(anchor);
    cursor.window_start = window_start;
    cursor.window_end = window_end;
    cursor.start = start;
    cursor.clip_id = pulp::timeline::ItemId{clip_id};
    return cursor;
}


namespace {

/// Opens a project and publishes it into a session so it has a revision.
///
/// AgentView admits a `DocumentView`, which pairs a snapshot with the revision
/// that produced it. A project read from disk has no revision of its own, so a
/// session is what supplies one. The session is discarded afterwards: the view
/// holds a shared snapshot, so the projection outlives it.
runtime::Result<pulp::timeline::DocumentView, OperationResult>
open_document_view(const ProjectSource& project,
                   const pulp::timeline::SchemaRegistry& registry) {
    auto loaded = detail::load_project(project, registry);
    if (!loaded)
        return runtime::Err(
            failure("open", persistence_message(loaded.error()), loaded.error().path));
    auto session = pulp::timeline::DocumentSession::create(std::move(loaded).value().value);
    if (!session)
        return runtime::Err(failure("view", "could not create a document session"));
    return runtime::Ok(session.value()->current());
}

} // namespace

OperationResult view_outline(const ProjectSource& project) {
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return failure("registry", "could not construct the built-in schema registry");
    auto opened = open_document_view(project, registry.value());
    if (!opened)
        return std::move(opened).error();
    const auto revision = opened.value().revision;
    auto view = av::AgentView::create(std::move(opened).value());
    if (!view)
        return {1, agent_view_error_json("view", view.error())};
    auto outline = view.value().outline(revision);
    if (!outline)
        return {1, agent_view_error_json("outline", outline.error())};
    return {0, agent_view_outline_json(outline.value())};
}

OperationResult view_region(const ProjectSource& project, const RegionViewOptions& options) {
    if (options.sequence_id == 0)
        return failure("region", "sequence_id must name a sequence", {}, 2);
    if (options.end <= options.start)
        return failure("region", "end must be greater than start", {}, 2);
    if (options.limit == 0)
        return failure("region", "limit must be at least 1", {}, 2);

    av::RegionRequest request;
    request.sequence_id = pulp::timeline::ItemId{options.sequence_id};
    request.anchor = options.absolute ? pulp::timeline::ClipTimeAnchor::Absolute
                                      : pulp::timeline::ClipTimeAnchor::Musical;
    request.start = options.start;
    request.end = options.end;
    request.limit = options.limit;
    if (!options.after.empty()) {
        auto cursor = parse_region_cursor_token(options.after);
        if (!cursor)
            return failure("region", "after is not a continuation token from a prior page", {}, 2);
        request.after = *cursor;
    }

    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return failure("registry", "could not construct the built-in schema registry");
    auto opened = open_document_view(project, registry.value());
    if (!opened)
        return std::move(opened).error();
    request.expected_revision = opened.value().revision;
    auto view = av::AgentView::create(std::move(opened).value());
    if (!view)
        return {1, agent_view_error_json("view", view.error())};
    auto page = view.value().region(request);
    if (!page)
        return {1, agent_view_error_json("region", page.error())};
    return {0, agent_view_region_page_json(page.value())};
}

OperationResult view_diff(const ProjectSource& project, std::string_view commands,
                          const WriterProfile& profile) {
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return failure("registry", "could not construct the built-in schema registry");
    auto loaded = detail::load_project(project, registry.value());
    if (!loaded)
        return failure("open", persistence_message(loaded.error()), loaded.error().path);
    auto decoded = pulp::timeline::deserialize_commands(commands, registry.value());
    if (!decoded)
        return failure("diff", persistence_message(decoded.error()), decoded.error().path, 2);
    auto session = pulp::timeline::DocumentSession::create(std::move(loaded).value().value);
    if (!session)
        return failure("diff", "could not create a document session");
    auto writer = session.value()->register_writer(profile.mask);
    if (!writer)
        return failure("diff", "could not register a document writer");

    pulp::timeline::Transaction transaction;
    transaction.id = writer.value().allocate_transaction_id();
    const auto before = session.value()->revision();
    transaction.expected_revision = before;
    transaction.commands.reserve(decoded.value().size());
    for (auto& command : decoded.value())
        transaction.commands.push_back({writer.value().allocate_command_id(), std::move(command)});
    const auto authorities = capture_command_authorities(transaction);
    auto committed = session.value()->submit(writer.value(), std::move(transaction));
    if (!committed)
        return {1, transaction_refusal_json(committed.error(), "diff", authorities)};

    auto view = av::AgentView::create(session.value()->current());
    if (!view)
        return {1, agent_view_error_json("view", view.error())};
    const av::DirtyRevisionRange revisions{before, committed.value().revision};
    auto diff = view.value().diff(committed.value().revision, revisions, committed.value().dirty);
    if (!diff)
        return {1, agent_view_error_json("diff", diff.error())};
    return {0, agent_view_outline_diff_json(diff.value())};
}

} // namespace pulp::tools::timeline
