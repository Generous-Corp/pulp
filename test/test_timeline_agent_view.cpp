#include <pulp/timeline_agent_view/agent_view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace pulp;

namespace {

constexpr std::size_t kPagingTrackCount = 100;
constexpr std::size_t kPagingClipsPerTrack = 100;
constexpr std::uint64_t kPagingFirstTrackId = 10'000;
constexpr std::uint64_t kPagingFirstClipId = 20'000;

template <class T, class E> T take(runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

timeline::Clip make_clip(std::uint64_t id, std::int64_t start,
                         std::optional<std::uint16_t> note_velocity = std::nullopt) {
    timeline::ClipContent content = timeline::EmptyContent{};
    if (note_velocity) {
        timeline::MidiExpressionLane lane{{200}, {}, {{{201}, {4}, 0x12345678u}}};
        content = take(timeline::MidiContent::create(
            {{{id + 100}, {3}, {8}, *note_velocity, 64, 0}}, {}, 99, {std::move(lane)}));
    }
    return take(timeline::Clip::create({id}, {start}, {10}, std::move(content)));
}

timeline::Project project_with(std::vector<std::int64_t> starts = {30, 10, 20},
                               std::string track_name = "track",
                               std::uint16_t note_velocity = 1000,
                               float track_gain = 1.0f,
                               std::int64_t sequence_duration = 1000,
                               std::uint64_t next_item_id = 1000) {
    std::vector<timeline::Clip> clips;
    for (std::size_t i = 0; i < starts.size(); ++i)
        clips.push_back(make_clip(10 + i, starts[i], i == 0 ? std::optional(note_velocity)
                                                           : std::nullopt));
    auto curve = take(timeline::AutomationCurve::create(
        {{{301}, {5}, 0.25f, timeline::AutomationInterpolation::Continuous, 0.0f}}));
    auto automation = take(timeline::AutomationLane::create(
        {300}, timeline::TrackMixerTarget{}, std::move(curve)));
    auto track = take(timeline::Track::create({.id = {3},
                                               .name = std::move(track_name),
                                               .clips = std::move(clips),
                                               .device_chain = {{{302}}},
                                               .automation_lanes = {std::move(automation)},
                                               .mixer = {track_gain, 0.0f}}));
    auto chord = take(timeline::ChordScaleLane::create({{{7}}}));
    timeline::Scene scene{{402}, "scene", {timeline::Slot{{403}, {10}, {}, {}}}};
    auto sequence = take(timeline::Sequence::create({.id = {2},
                                                     .name = "sequence",
                                                     .musical_duration =
                                                         timebase::TickDuration{sequence_duration},
                                                     .tracks = {std::move(track)},
                                                     .markers = {{{400}, "marker", {8}, {}}},
                                                     .regions = {{{401}, "region", {9}, {10}, {}}},
                                                     .chord_scale_lane = std::move(chord),
                                                     .scenes = {std::move(scene)}}));
    const auto asset_hash = *timeline::ContentHash::from_hex(std::string(64, 'a'));
    return take(timeline::Project::create({.id = {1},
                                           .name = "project",
                                           .next_item_id = next_item_id,
                                           .root_sequence_id = {2},
                                           .assets = {{{500}, "asset.wav", 100, {48'000, 1},
                                                       asset_hash,
                                                       timeline::AssetStoragePolicy::External,
                                                       {},
                                                       {},
                                                       {}}},
                                           .sequences = {std::move(sequence)}}));
}

timeline::Project paging_scale_project() {
    std::vector<timeline::Track> tracks;
    tracks.reserve(kPagingTrackCount);
    for (std::size_t track_index = 0; track_index < kPagingTrackCount; ++track_index) {
        std::vector<timeline::Clip> clips;
        clips.reserve(kPagingClipsPerTrack);
        for (std::size_t clip_index = 0; clip_index < kPagingClipsPerTrack; ++clip_index) {
            const auto id = kPagingFirstClipId + track_index * kPagingClipsPerTrack + clip_index;
            const auto interleaved =
                clip_index * kPagingTrackCount + (track_index * 37) % kPagingTrackCount;
            clips.push_back(take(timeline::Clip::create(
                {id}, {static_cast<std::int64_t>(2 * interleaved)}, {1},
                timeline::EmptyContent{})));
        }
        tracks.push_back(take(timeline::Track::create(
            {kPagingFirstTrackId + track_index}, "paging-track", std::move(clips))));
    }
    auto sequence = take(timeline::Sequence::create(
        {.id = {2}, .name = "paging-sequence", .tracks = std::move(tracks)}));
    return take(timeline::Project::create(
        {.id = {1},
         .name = "paging-scale",
         .next_item_id = kPagingFirstClipId + kPagingTrackCount * kPagingClipsPerTrack,
         .root_sequence_id = {2},
         .sequences = {std::move(sequence)}}));
}

timeline_agent_view::AgentView pin(timeline::Project project,
                                   timeline::DocumentRevision revision = {7}) {
    return take(timeline_agent_view::AgentView::create(
        {std::make_shared<const timeline::Project>(std::move(project)), revision}));
}

std::size_t projected_omissions(const timeline_agent_view::Outline& outline) {
    std::size_t total = outline.omitted.count;
    for (const auto& sequence : outline.sequences) {
        total += sequence.omitted.count;
        for (const auto& track : sequence.tracks) {
            total += track.omitted.count;
            for (const auto& clip : track.clips)
                total += clip.omitted.count;
        }
    }
    return total;
}

std::size_t census_omissions(const timeline::ProjectSnapshotCounts& c) {
    return c.assets + c.notes + c.device_placements + c.automation_lanes +
           c.automation_points + c.take_lanes + c.takes + c.take_comp_segments + c.markers +
           c.regions + c.scenes + c.slots + c.chord_scale_events + c.groove_steps +
           c.midi_lanes + c.midi_lane_points;
}

bool strict_performance() {
    const auto* value = std::getenv("PULP_PERF_STRICT");
    return value && value[0] && value[0] != '0';
}

std::optional<std::chrono::milliseconds> performance_budget(const char* name) {
    const auto* value = std::getenv(name);
    if (!value || !value[0]) {
        INFO("missing performance budget: " << name);
        REQUIRE_FALSE(strict_performance());
        return std::nullopt;
    }

    const std::string_view text(value);
    std::chrono::milliseconds::rep milliseconds = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), milliseconds);
    INFO("invalid performance budget " << name << '=' << text);
    REQUIRE(parsed.ec == std::errc{});
    REQUIRE(parsed.ptr == text.data() + text.size());
    REQUIRE(milliseconds > 0);
    return std::chrono::milliseconds(milliseconds);
}

template <class Rep, class Period>
void enforce_performance_budget(const char* name,
                                std::chrono::duration<Rep, Period> elapsed) {
    const auto budget = performance_budget(name);
    if (!budget)
        return;
    INFO(name << " elapsed_us="
              << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()
              << " budget_ms=" << budget->count());
    REQUIRE(elapsed <= *budget);
}

} // namespace

TEST_CASE("AgentView omission partitions equal the independent structural census") {
    const auto project = project_with();
    const auto outline = take(pin(project).outline({7}));
    const auto registry = take(timeline::make_builtin_timeline_registry());
    const auto serialized = take(timeline::serialize_project(project, registry));
    const auto oracle = take(timeline::peek_project_summary(serialized.json, registry)).counts;
    REQUIRE(outline.explicit_item_count == 6);
    REQUIRE(projected_omissions(outline) == census_omissions(outline.census));
    CHECK(outline.census.assets == oracle.assets);
    CHECK(outline.census.sequences == oracle.sequences);
    CHECK(outline.census.tracks == oracle.tracks);
    CHECK(outline.census.clips == oracle.clips);
    CHECK(outline.census.notes == oracle.notes);
    CHECK(outline.census.device_placements == oracle.device_placements);
    CHECK(outline.census.automation_lanes == oracle.automation_lanes);
    CHECK(outline.census.automation_points == oracle.automation_points);
    CHECK(outline.census.take_lanes == oracle.take_lanes);
    CHECK(outline.census.takes == oracle.takes);
    CHECK(outline.census.take_comp_segments == oracle.take_comp_segments);
    CHECK(outline.census.markers == oracle.markers);
    CHECK(outline.census.regions == oracle.regions);
    CHECK(outline.census.scenes == oracle.scenes);
    CHECK(outline.census.slots == oracle.slots);
    CHECK(outline.census.chord_scale_events == oracle.chord_scale_events);
    CHECK(outline.census.groove_steps == oracle.groove_steps);
    CHECK(outline.census.midi_lanes == oracle.midi_lanes);
    CHECK(outline.census.midi_lane_points == oracle.midi_lane_points);
    REQUIRE(oracle.notes == 1);
    const auto& clips = outline.sequences[0].tracks[0].clips;
    const auto note_clip = std::find_if(clips.begin(), clips.end(), [](const auto& clip) {
        return clip.id == timeline::ItemId{10};
    });
    REQUIRE(note_clip != clips.end());
    REQUIRE(note_clip->omitted.count == 3);
}

TEST_CASE("AgentView omission hashes ignore visible rows and detect omitted mutations") {
    const auto base = take(pin(project_with({30, 10, 20}, "track", 1000)).outline({7}));
    const auto renamed = take(pin(project_with({30, 10, 20}, "renamed", 1000)).outline({7}));
    const auto mutated = take(pin(project_with({30, 10, 20}, "track", 1001)).outline({7}));
    const auto mixed =
        take(pin(project_with({30, 10, 20}, "track", 1000, 0.5f)).outline({7}));
    const auto resized =
        take(pin(project_with({30, 10, 20}, "track", 1000, 1.0f, 1001)).outline({7}));
    const auto frontier =
        take(pin(project_with({30, 10, 20}, "track", 1000, 1.0f, 1000, 1001))
                 .outline({7}));

    const auto& base_track = base.sequences[0].tracks[0];
    const auto& renamed_track = renamed.sequences[0].tracks[0];
    REQUIRE(base_track.omitted.sha256 == renamed_track.omitted.sha256);
    REQUIRE(base_track.content_sha256 != renamed_track.content_sha256);
    REQUIRE(base_track.omitted.sha256 != mixed.sequences[0].tracks[0].omitted.sha256);
    REQUIRE(base.sequences[0].omitted.sha256 != resized.sequences[0].omitted.sha256);
    REQUIRE(base.omitted.sha256 != frontier.omitted.sha256);

    const auto find_note_clip = [](const auto& track) -> const auto& {
        return *std::find_if(track.clips.begin(), track.clips.end(), [](const auto& clip) {
            return clip.id == timeline::ItemId{10};
        });
    };
    const auto& base_clip = find_note_clip(base_track);
    const auto& mutated_clip = find_note_clip(mutated.sequences[0].tracks[0]);
    REQUIRE(base_clip.omitted.sha256 != mutated_clip.omitted.sha256);
    REQUIRE(base_clip.content_sha256 != mutated_clip.content_sha256);
}

TEST_CASE("AgentView region cursors exhaust canonical start-id order without overlap") {
    std::mt19937_64 random(0xA63E17u);
    for (std::size_t trial = 0; trial < 32; ++trial) {
        std::vector<std::int64_t> starts;
        for (std::size_t i = 0; i < 12; ++i)
            starts.push_back(static_cast<std::int64_t>(i * 20));
        std::shuffle(starts.begin(), starts.end(), random);
        auto view = pin(project_with(starts));
        timeline_agent_view::RegionRequest request{{7}, {2}, timeline::ClipTimeAnchor::Musical,
                                                   0, 300, 1 + trial % 5, {}};
        std::vector<std::pair<std::int64_t, std::uint64_t>> observed;
        do {
            auto page = take(view.region(request));
            for (const auto& item : page.items)
                observed.emplace_back(item.start, item.id.value);
            request.after = page.next;
        } while (request.after);
        REQUIRE(observed.size() == starts.size());
        REQUIRE(std::is_sorted(observed.begin(), observed.end()));
        REQUIRE(std::adjacent_find(observed.begin(), observed.end()) == observed.end());
    }
}

TEST_CASE("AgentView refuses stale revisions and cursors outside the requested window") {
    auto view = pin(project_with());
    const auto stale_outline = view.outline({8});
    REQUIRE_FALSE(stale_outline);
    REQUIRE(stale_outline.error().code == timeline_agent_view::ErrorCode::StaleRevision);

    timeline_agent_view::RegionRequest request{{7}, {2}, timeline::ClipTimeAnchor::Musical,
                                               0, 100, 1, {}};
    const auto first = take(view.region(request));
    REQUIRE(first.next);
    request.start = 15;
    request.after = first.next;
    const auto rejected = view.region(request);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == timeline_agent_view::ErrorCode::InvalidCursor);

    request = {{7}, {2}, timeline::ClipTimeAnchor::Musical, 0, 100, 1, {}};
    const auto bounded = take(view.region(request));
    REQUIRE(bounded.next);
    request.end = 200;
    request.after = bounded.next;
    const auto widened = view.region(request);
    REQUIRE_FALSE(widened);
    REQUIRE(widened.error().code == timeline_agent_view::ErrorCode::InvalidCursor);

    request = {{7}, {2}, timeline::ClipTimeAnchor::Musical, 0, 100, 1, bounded.next};
    request.start = -1;
    const auto shifted = view.region(request);
    REQUIRE_FALSE(shifted);
    REQUIRE(shifted.error().code == timeline_agent_view::ErrorCode::InvalidCursor);
}

TEST_CASE("AgentView DirtySet projection requires an adjacent transition ending at the pin") {
    auto view = pin(project_with());
    timeline::DirtySet dirty({{{110}, {3}, {2}, timeline::DirtyFlags::Notes},
                              {{3}, {3}, {2}, timeline::DirtyFlags::Content}});
    const auto projected = take(view.diff({7}, {{6}, {7}}, dirty));
    REQUIRE(projected.changes.size() == 2);
    REQUIRE(projected.changes[0].kind == timeline_agent_view::OutlineKind::Track);
    REQUIRE(projected.changes[1].kind == timeline_agent_view::OutlineKind::Clip);
    REQUIRE(projected.changes[1].item_id == timeline::ItemId{10});

    const auto wrong_transition = view.diff({7}, {{5}, {6}}, dirty);
    REQUIRE_FALSE(wrong_transition);
    REQUIRE(wrong_transition.error().code ==
            timeline_agent_view::ErrorCode::InvalidProvenance);

    const auto nonadjacent_transition = view.diff({7}, {{5}, {7}}, dirty);
    REQUIRE_FALSE(nonadjacent_transition);
    REQUIRE(nonadjacent_transition.error().code ==
            timeline_agent_view::ErrorCode::InvalidProvenance);

    auto zero_revision_view = pin(project_with(), {0});
    const auto wrapped_transition = zero_revision_view.diff(
        {0}, {{std::numeric_limits<std::uint64_t>::max()}, {0}}, dirty);
    REQUIRE_FALSE(wrapped_transition);
    REQUIRE(wrapped_transition.error().code ==
            timeline_agent_view::ErrorCode::InvalidProvenance);
}

TEST_CASE("AgentView bounds work and rejects untrusted dirty ownership") {
    const auto project = project_with();
    const timeline::DocumentView document{
        std::make_shared<const timeline::Project>(project), {7}};
    const auto too_many_rows = timeline_agent_view::AgentView::create(
        document, {.max_outline_items = 5, .max_page_items = 2, .max_canonical_bytes = 4096});
    REQUIRE_FALSE(too_many_rows);
    REQUIRE(too_many_rows.error().code == timeline_agent_view::ErrorCode::LimitExceeded);

    const auto too_many_census_rows = timeline_agent_view::AgentView::create(
        document, {.max_outline_items = 10,
                   .max_census_items = 5,
                   .max_page_items = 2,
                   .max_canonical_bytes = 4096});
    REQUIRE_FALSE(too_many_census_rows);
    REQUIRE(too_many_census_rows.error().code ==
            timeline_agent_view::ErrorCode::LimitExceeded);

    auto byte_bounded = take(timeline_agent_view::AgentView::create(
        document, {.max_outline_items = 10, .max_page_items = 2, .max_canonical_bytes = 32}));
    const auto rejected_outline = byte_bounded.outline({7});
    REQUIRE_FALSE(rejected_outline);
    REQUIRE(rejected_outline.error().code == timeline_agent_view::ErrorCode::LimitExceeded);

    auto view = pin(project);
    const auto overlarge_page = view.region(
        {{7}, {2}, timeline::ClipTimeAnchor::Musical, 0, 100, 1001, {}});
    REQUIRE_FALSE(overlarge_page);
    REQUIRE(overlarge_page.error().code == timeline_agent_view::ErrorCode::LimitExceeded);

    timeline::DirtySet forged(
        {{{110}, {999}, {2}, timeline::DirtyFlags::Notes}});
    const auto rejected_dirty = view.diff({7}, {{6}, {7}}, forged);
    REQUIRE_FALSE(rejected_dirty);
    REQUIRE(rejected_dirty.error().code == timeline_agent_view::ErrorCode::InvalidDirtySet);
}

TEST_CASE("AgentView outlines and canonically pages ten thousand interleaved clips",
          "[timeline][agent-view][scale][performance]") {
    auto project = paging_scale_project();
    const auto outline_started = std::chrono::steady_clock::now();
    auto view = take(timeline_agent_view::AgentView::create(
        {std::make_shared<const timeline::Project>(std::move(project)), {7}}));
    const auto outline = take(view.outline({7}));
    const auto outline_elapsed = std::chrono::steady_clock::now() - outline_started;

    constexpr auto expected_clips = kPagingTrackCount * kPagingClipsPerTrack;
    REQUIRE(outline.explicit_item_count == 2 + kPagingTrackCount + expected_clips);
    REQUIRE(outline.census.sequences == 1);
    REQUIRE(outline.census.tracks == kPagingTrackCount);
    REQUIRE(outline.census.clips == expected_clips);
    REQUIRE(outline.sequences.size() == 1);
    REQUIRE(outline.sequences[0].tracks.size() == kPagingTrackCount);

    timeline_agent_view::RegionRequest request{{7}, {2},
                                               timeline::ClipTimeAnchor::Musical,
                                               0,
                                               static_cast<std::int64_t>(2 * expected_clips),
                                               1'000,
                                               {}};
    std::vector<std::pair<std::int64_t, std::uint64_t>> observed;
    observed.reserve(expected_clips);
    std::vector<std::uint64_t> observed_ids;
    observed_ids.reserve(expected_clips);
    std::size_t pages = 0;
    const auto paging_started = std::chrono::steady_clock::now();
    do {
        const auto page = take(view.region(request));
        ++pages;
        for (const auto& item : page.items) {
            observed.emplace_back(item.start, item.id.value);
            observed_ids.push_back(item.id.value);
        }
        request.after = page.next;
    } while (request.after);
    const auto paging_elapsed = std::chrono::steady_clock::now() - paging_started;

    REQUIRE(pages == 10);
    REQUIRE_FALSE(request.after);
    REQUIRE(observed.size() == expected_clips);
    REQUIRE(std::is_sorted(observed.begin(), observed.end()));
    REQUIRE(std::adjacent_find(observed.begin(), observed.end()) == observed.end());
    std::sort(observed_ids.begin(), observed_ids.end());
    REQUIRE(std::adjacent_find(observed_ids.begin(), observed_ids.end()) == observed_ids.end());
    REQUIRE(observed.front().first == 0);
    REQUIRE(observed.back().first == static_cast<std::int64_t>(2 * (expected_clips - 1)));

    if (strict_performance()) {
        enforce_performance_budget("PULP_TIMELINE_AGENT_VIEW_OUTLINE_BUDGET_MS",
                                   outline_elapsed);
        enforce_performance_budget("PULP_TIMELINE_AGENT_VIEW_PAGING_BUDGET_MS",
                                   paging_elapsed);
    }
}
