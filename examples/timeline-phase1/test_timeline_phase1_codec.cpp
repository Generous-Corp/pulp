#include "timeline_phase1_example_test_support.hpp"

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

    auto mutable_document = std::make_shared<StepPatternDocument>();
    mutable_document->snapshot = source.pattern_snapshot();
    std::shared_ptr<const void> erased = mutable_document;
    auto registered = source.pattern_registry().create_registered_no_owned_ids(
        {kStepPatternSchemaName, kStepPatternSchemaVersion}, std::move(erased),
        2u * 1024u * 1024u);
    REQUIRE(registered);
    mutable_document->snapshot.patterns[1].length = 1;
    auto padded_project = project_with_step_pattern(std::move(registered).value());
    REQUIRE(padded_project);
    TimelineStepSequencerProcessor loader;
    loader.prepare(prepare_context());
    REQUIRE_FALSE(loader.load_persistent_project(*padded_project));

    auto schema_document = std::make_shared<StepPatternDocument>();
    schema_document->snapshot = source.pattern_snapshot();
    std::shared_ptr<const void> schema_erased = schema_document;
    auto schema_registered = source.pattern_registry().create_registered_no_owned_ids(
        {kStepPatternSchemaName, kStepPatternSchemaVersion},
        std::move(schema_erased), 2u * 1024u * 1024u);
    REQUIRE(schema_registered);
    schema_document->snapshot.schema_version = kStepPatternSchemaVersion + 1;
    auto wrong_schema_project =
        project_with_step_pattern(std::move(schema_registered).value());
    REQUIRE(wrong_schema_project);
    REQUIRE_FALSE(loader.load_persistent_project(*wrong_schema_project));
}
