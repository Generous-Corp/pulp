#include "timeline_phase1_example_test_support.hpp"

namespace {

enum class InvalidStepProject { InactivePatternPadding, SnapshotSchema };

std::optional<timeline::Project>
make_invalid_step_project(const TimelineStepSequencerProcessor& source,
                          InvalidStepProject kind) {
    auto document = std::make_shared<StepPatternDocument>();
    document->snapshot = source.pattern_snapshot();
    std::shared_ptr<const void> erased = document;
    auto registered = source.pattern_registry().create_registered_no_owned_ids(
        {kStepPatternSchemaName, kStepPatternSchemaVersion}, std::move(erased),
        2u * 1024u * 1024u);
    if (!registered)
        return std::nullopt;

    // Mutate after registration to exercise load-time validation independently
    // of the registry's construction-time canonicalization checks.
    if (kind == InvalidStepProject::InactivePatternPadding)
        document->snapshot.patterns[1].length = 1;
    else
        document->snapshot.schema_version = kStepPatternSchemaVersion + 1;
    return project_with_step_pattern(std::move(registered).value());
}

void require_snapshots_equal(const state::Snapshot& actual,
                             const state::Snapshot& expected) {
    REQUIRE(actual.schema_version == expected.schema_version);
    REQUIRE(actual.epoch == expected.epoch);
    REQUIRE(actual.engine_sequence == expected.engine_sequence);
    REQUIRE(actual.active_pattern == expected.active_pattern);
    REQUIRE(actual.active_lane_count == expected.active_lane_count);
    REQUIRE(actual.active_pattern_count == expected.active_pattern_count);
    for (std::uint8_t pattern = 0; pattern < state::kPatternCount; ++pattern) {
        REQUIRE(actual.patterns[pattern].length == expected.patterns[pattern].length);
        for (std::uint8_t lane = 0; lane < state::kLaneCount; ++lane)
            for (std::uint8_t step = 0; step < state::kStepCount; ++step)
                REQUIRE(cells_equal(actual.patterns[pattern].lanes[lane][step],
                                    expected.patterns[pattern].lanes[lane][step]));
    }
}

} // namespace

TEST_CASE("timeline step pattern codec is canonical at maximum bounded extent") {
    auto registry = make_step_pattern_registry();
    REQUIRE(registry);
    state::Snapshot maximum;
    maximum.schema_version = kStepPatternSchemaVersion;
    maximum.active_pattern = state::kPatternCount - 1;
    maximum.active_lane_count = state::kLaneCount;
    maximum.active_pattern_count = state::kPatternCount;
    for (std::uint8_t pattern = 0; pattern < state::kPatternCount; ++pattern) {
        maximum.patterns[pattern].length =
            pattern % 2 == 0 ? state::kStepCount : 0;
        for (std::uint8_t lane = 0; lane < state::kLaneCount; ++lane) {
            for (std::uint8_t step = 0; step < state::kStepCount; ++step) {
                auto& cell = maximum.patterns[pattern].lanes[lane][step];
                cell.flags = static_cast<std::uint8_t>((pattern + lane + step) & 0xffu);
                cell.velocity = static_cast<std::uint8_t>((pattern + lane + step) % 128u);
                cell.probability = static_cast<std::uint8_t>(127u - (step % 128u));
                const auto pitch_ordinal =
                    (static_cast<unsigned>(pattern) * state::kLaneCount + lane + step) %
                    256u;
                cell.pitch_offset = static_cast<std::int8_t>(
                    static_cast<int>(pitch_ordinal) - 128);
                cell.gate_ticks = static_cast<std::uint16_t>(
                    (static_cast<unsigned>(pattern) * 997u + lane * 37u + step) & 0xffffu);
                cell.ratchet = static_cast<std::uint8_t>((lane + step) & 0xffu);
                cell.reserved = 0;
            }
        }
    }
    REQUIRE(step_pattern_snapshot_is_canonical(maximum));
    auto registered = make_registered_step_pattern(maximum, *registry);
    REQUIRE(registered);
    const auto canonical = registered->canonical_payload_json();
    auto project = project_with_step_pattern(std::move(*registered));
    REQUIRE(project);
    auto encoded = timeline::serialize_project(*project, *registry);
    REQUIRE(encoded);
    auto decoded = timeline::deserialize_project(encoded.value().json, *registry);
    REQUIRE(decoded);
    const auto* clip = decoded.value().find_sequence({3})->find_track({4})->find_clip({5});
    REQUIRE(clip);
    const auto* content = std::get_if<timeline::RegisteredContent>(&clip->content());
    REQUIRE(content);
    REQUIRE(content->canonical_payload_json() == canonical);
    const auto* roundtrip = content->value_as<StepPatternDocument>();
    REQUIRE(roundtrip);
    REQUIRE(roundtrip->snapshot.active_pattern == maximum.active_pattern);
    REQUIRE(roundtrip->snapshot.active_lane_count == state::kLaneCount);
    REQUIRE(roundtrip->snapshot.active_pattern_count == state::kPatternCount);
    for (std::uint8_t pattern = 0; pattern < state::kPatternCount; ++pattern) {
        REQUIRE(roundtrip->snapshot.patterns[pattern].length ==
                maximum.patterns[pattern].length);
        for (std::uint8_t lane = 0; lane < state::kLaneCount; ++lane)
            for (std::uint8_t step = 0; step < state::kStepCount; ++step)
                REQUIRE(cells_equal(roundtrip->snapshot.patterns[pattern].lanes[lane][step],
                                    maximum.patterns[pattern].lanes[lane][step]));
    }
}
TEST_CASE("timeline step pattern codec rejects malformed schema counts cells and padding") {
    TimelineStepSequencerProcessor source;
    source.prepare(prepare_context());
    auto encoded = timeline::serialize_project(*source.persistent_project(),
                                               source.pattern_registry());
    REQUIRE(encoded);
    const std::array mutations{
        std::pair{std::string_view{"\"schema_version\":1"},
                  std::string_view{"\"schema_version\":2"}},
        std::pair{std::string_view{"\"active_lane_count\":4"},
                  std::string_view{"\"active_lane_count\":5"}},
        std::pair{std::string_view{"[1,100,127,\"0\",12,1]"},
                  std::string_view{"[1,100,128,\"0\",12,1]"}},
    };
    for (const auto& [before, after] : mutations) {
        auto malformed = encoded.value().json;
        REQUIRE(replace_once(malformed, before, after));
        REQUIRE_FALSE(timeline::deserialize_project(malformed,
                                                    source.pattern_registry()));
    }

    auto invalid_version = source.pattern_snapshot();
    invalid_version.schema_version = kStepPatternSchemaVersion + 1;
    REQUIRE_FALSE(make_registered_step_pattern(invalid_version,
                                               source.pattern_registry()));

    auto inactive_lane = source.pattern_snapshot();
    inactive_lane.patterns[0].lanes[inactive_lane.active_lane_count][0].flags =
        state::StepCell::kEnabledBit;
    REQUIRE_FALSE(step_pattern_snapshot_is_canonical(inactive_lane));
    REQUIRE_FALSE(make_registered_step_pattern(inactive_lane,
                                               source.pattern_registry()));

    auto padded_project = make_invalid_step_project(
        source, InvalidStepProject::InactivePatternPadding);
    REQUIRE(padded_project);
    TimelineStepSequencerProcessor loader;
    loader.prepare(prepare_context());
    REQUIRE_FALSE(loader.load_persistent_project(*padded_project));

    auto wrong_schema_project = make_invalid_step_project(
        source, InvalidStepProject::SnapshotSchema);
    REQUIRE(wrong_schema_project);
    REQUIRE_FALSE(loader.load_persistent_project(*wrong_schema_project));
}

TEST_CASE("timeline step validation-rejected project loads preserve the live generation") {
    TimelineStepSequencerProcessor processor;
    processor.prepare(prepare_context());
    REQUIRE(processor.engine_prepared());
    REQUIRE(submit_pitch_edit(processor, 12));
    REQUIRE(processor.apply_pending_edits_and_recompile());
    REQUIRE(processor.pattern_snapshot().patterns[0].lanes[0][0].pitch_offset == 12);

    const auto pattern_before = processor.pattern_snapshot();
    const auto channel_before = processor.channel().ui_read_latest_snapshot();
    const auto resync_before = processor.channel().ui_resync_required_epoch();
    auto project_before = timeline::serialize_project(*processor.persistent_project(),
                                                      processor.pattern_registry());
    REQUIRE(project_before);
    REQUIRE(processor.set_playing(true) == playback::TransportError::None);
    REQUIRE(processor.seek_samples(0) == playback::TransportError::None);
    StereoBlock render_before(128);
    process_direct(processor, render_before);
    REQUIRE(render_before.energy() > 0.0);

    const std::array invalid_projects{
        make_invalid_step_project(processor,
                                  InvalidStepProject::InactivePatternPadding),
        make_invalid_step_project(processor, InvalidStepProject::SnapshotSchema),
    };
    for (const auto& invalid : invalid_projects) {
        REQUIRE(invalid);
        REQUIRE_FALSE(processor.load_persistent_project(*invalid));

        require_snapshots_equal(processor.pattern_snapshot(), pattern_before);
        auto project_after = timeline::serialize_project(*processor.persistent_project(),
                                                         processor.pattern_registry());
        REQUIRE(project_after);
        REQUIRE(project_after->json == project_before->json);
        require_snapshots_equal(processor.channel().ui_read_latest_snapshot(),
                                channel_before);
        REQUIRE(processor.channel().ui_resync_required_epoch() == resync_before);
        REQUIRE(processor.engine_prepared());

        REQUIRE(processor.seek_samples(0) == playback::TransportError::None);
        StereoBlock render_after(128);
        process_direct(processor, render_after);
        REQUIRE(render_after.left == render_before.left);
        REQUIRE(render_after.right == render_before.right);
    }
}
