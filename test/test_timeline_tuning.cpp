#include "support/timeline_persistence_test_support.hpp"

namespace {

TuningReference scala_tuning() {
    TuningReference tuning;
    tuning.system = TuningSystem::Scala;
    tuning.reference_pitch_millihertz = 415'300;
    tuning.scale_content = hash('a');
    tuning.keyboard_map_content = hash('b');
    return tuning;
}

TuningReference mts_tuning() {
    TuningReference tuning;
    tuning.system = TuningSystem::MtsEsp;
    tuning.reference_pitch_millihertz = 432'000;
    return tuning;
}

// Ids: project 1, sequence 2, track 3. Either tuning may be absent, so one
// helper covers "project only", "track only", "both", and "neither".
Project tuned_project(std::optional<TuningReference> project_tuning,
                      std::optional<TuningReference> track_tuning) {
    TrackInput track_input;
    track_input.id = {3};
    track_input.name = "lead";
    track_input.tuning = std::move(track_tuning);
    auto track = take(Track::create(std::move(track_input)));
    auto sequence = take(Sequence::create({2}, "root", TickDuration{100}, {track}));
    ProjectInput input;
    input.id = {1};
    input.name = "tuned";
    input.next_item_id = 4;
    input.root_sequence_id = {2};
    input.sequences = {sequence};
    input.tuning = std::move(project_tuning);
    return take(Project::create(std::move(input)));
}

std::uint32_t current_track_version() {
    return current_schema_version("pulp.timeline.track");
}

std::string track_envelope(const std::string& project_json) {
    const auto begin = project_json.find(R"({"data":{"active_take_lane_id")");
    REQUIRE(begin != std::string::npos);
    // The encoder writes whatever version is current, which is not the version
    // tuning was introduced at once another slice extends the track. Deriving
    // the suffix keeps this helper from silently failing to find an envelope
    // that is present, which reads as "no track here" rather than as a bump.
    const auto suffix = ",\"type_name\":\"pulp.timeline.track\",\"version\":" +
                        std::to_string(current_track_version()) + "}";
    const auto end = project_json.find(suffix, begin);
    REQUIRE(end != std::string::npos);
    return project_json.substr(begin, end - begin + suffix.size());
}

} // namespace

TEST_CASE("a project tuning reference round trips", "[timeline][tuning][persistence]") {
    const auto registry = builtins();
    const auto project = tuned_project(scala_tuning(), std::nullopt);
    const auto encoded = take(serialize_project(project, registry)).json;
    const auto decoded = take(deserialize_project(encoded, registry));

    REQUIRE(decoded.tuning().has_value());
    REQUIRE(decoded.tuning()->system == TuningSystem::Scala);
    // The reference pitch is exact millihertz, so a document states the pitch
    // the user typed rather than the nearest float.
    REQUIRE(decoded.tuning()->reference_pitch_millihertz == 415'300);
    REQUIRE(decoded.tuning()->scale_content == hash('a'));
    REQUIRE(decoded.tuning()->keyboard_map_content == hash('b'));
    REQUIRE(take(serialize_project(decoded, registry)).json == encoded);

    // A project that states no tuning omits the member entirely; absence is not
    // the same claim as stating equal temperament.
    const auto untuned = take(serialize_project(tuned_project(std::nullopt, std::nullopt),
                                                registry))
                             .json;
    REQUIRE(untuned.find(R"("tuning")") == std::string::npos);
    REQUIRE_FALSE(take(deserialize_project(untuned, registry)).tuning().has_value());
}

TEST_CASE("a per-instrument tuning reference round trips beside the project's",
          "[timeline][tuning][persistence]") {
    const auto registry = builtins();
    const auto project = tuned_project(scala_tuning(), mts_tuning());
    const auto encoded = take(serialize_project(project, registry)).json;
    const auto decoded = take(deserialize_project(encoded, registry));

    const auto* track = decoded.find_sequence({2})->tracks()[0].tuning()
                            ? &decoded.find_sequence({2})->tracks()[0]
                            : nullptr;
    REQUIRE(track != nullptr);
    REQUIRE(track->tuning()->system == TuningSystem::MtsEsp);
    REQUIRE(track->tuning()->reference_pitch_millihertz == 432'000);
    REQUIRE(decoded.tuning()->system == TuningSystem::Scala);
    REQUIRE(take(serialize_project(decoded, registry)).json == encoded);
}

TEST_CASE("an inconsistent tuning reference is refused", "[timeline][tuning]") {
    // A Scala tuning with no scale payload names nothing.
    TuningReference no_payload;
    no_payload.system = TuningSystem::Scala;
    REQUIRE_FALSE(valid_tuning_reference(no_payload));
    const auto rejected_project = Project::create([&] {
        auto input = ProjectInput{};
        input.id = {1};
        input.name = "bad";
        input.next_item_id = 4;
        input.root_sequence_id = {2};
        input.sequences = {take(Sequence::create(
            {2}, "root", TickDuration{100}, {take(Track::create({3}, "lead", {}))}))};
        input.tuning = no_payload;
        return input;
    }());
    REQUIRE_FALSE(rejected_project);
    REQUIRE(rejected_project.error().code == ModelErrorCode::InvalidTuningReference);

    // A non-Scala system carrying a payload is refused rather than having the
    // payload ignored: a reader that ignored it would play a different scale.
    TuningReference stray_payload = mts_tuning();
    stray_payload.scale_content = hash('a');
    REQUIRE_FALSE(valid_tuning_reference(stray_payload));

    // The reference pitch is bounded on both sides; zero is not a pitch.
    TuningReference silent;
    silent.reference_pitch_millihertz = 0;
    REQUIRE_FALSE(valid_tuning_reference(silent));
    TuningReference ultrasonic;
    ultrasonic.reference_pitch_millihertz = kMaxReferencePitchMillihertz + 1;
    REQUIRE_FALSE(valid_tuning_reference(ultrasonic));

    // A track's tuning is validated on the same terms as the project's.
    TrackInput track_input;
    track_input.id = {3};
    track_input.name = "lead";
    track_input.tuning = no_payload;
    const auto rejected_track = Track::create(std::move(track_input));
    REQUIRE_FALSE(rejected_track);
    REQUIRE(rejected_track.error().code == ModelErrorCode::InvalidTuningReference);
}

TEST_CASE("the project tuning migration refuses to discard an authored tuning",
          "[timeline][tuning][migration]") {
    const auto registry = builtins();
    DecodeLimits limits;

    const auto tuned = take(serialize_project(tuned_project(scala_tuning(), std::nullopt),
                                              registry))
                           .json;
    // A tuning decides what every pitch sounds like, so a reader that silently
    // dropped it would play the whole project in some other scale.
    auto refused =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.project", 3, 2, tuned, limits);
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().code == PersistenceErrorCode::MigrationFailed);

    // A project that never chose a tuning downgrades cleanly, and the upgrade
    // puts back exactly the bytes it removed.
    const auto plain =
        take(serialize_project(tuned_project(std::nullopt, std::nullopt), registry)).json;
    const auto lowered = take(
        registry.migrate(SchemaDomain::Document, "pulp.timeline.project", 3, 2, plain, limits));
    REQUIRE(lowered.find(R"("type_name":"pulp.timeline.project","version":2)") !=
            std::string::npos);
    const auto raised = take(
        registry.migrate(SchemaDomain::Document, "pulp.timeline.project", 2, 3, lowered, limits));
    REQUIRE(raised == plain);
}

TEST_CASE("the track tuning migration refuses to discard an instrument override",
          "[timeline][tuning][migration]") {
    const auto registry = builtins();
    DecodeLimits limits;

    const auto overridden =
        take(serialize_project(tuned_project(std::nullopt, mts_tuning()), registry)).json;
    // Down from whatever is current to the version before tuning existed. The
    // chain walks every intermediate step, so the tuning step is still the one
    // that must refuse.
    auto refused = registry.migrate(SchemaDomain::Document, "pulp.timeline.track",
                                    current_track_version(), 7, track_envelope(overridden),
                                    limits);
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().code == PersistenceErrorCode::MigrationFailed);

    const auto plain =
        take(serialize_project(tuned_project(std::nullopt, std::nullopt), registry)).json;
    const auto envelope = track_envelope(plain);
    const auto lowered = take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track",
                                              current_track_version(), 7, envelope, limits));
    REQUIRE(lowered.find(R"("type_name":"pulp.timeline.track","version":7)") != std::string::npos);
    const auto raised = take(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 7,
                                              current_track_version(), lowered, limits));
    REQUIRE(raised == envelope);
}

TEST_CASE("a payload carrying a tuning its version predates is refused",
          "[timeline][tuning][persistence]") {
    const auto registry = builtins();
    auto mismatched = take(serialize_project(tuned_project(scala_tuning(), std::nullopt),
                                             registry))
                          .json;
    constexpr std::string_view current = R"("type_name":"pulp.timeline.project","version":3)";
    const auto version = mismatched.find(current);
    REQUIRE(version != std::string::npos);
    mismatched.replace(version, current.size(),
                       R"("type_name":"pulp.timeline.project","version":2)");
    auto rejected = deserialize_project(mismatched, registry);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == PersistenceErrorCode::InvalidSchema);
}

TEST_CASE("a malformed tuning payload is refused rather than defaulted",
          "[timeline][tuning][persistence]") {
    const auto registry = builtins();
    const auto encoded =
        take(serialize_project(tuned_project(scala_tuning(), std::nullopt), registry)).json;

    // A truncated hash names no content. Decoding it as absent would silently
    // retune the document to whatever the fallback is.
    auto truncated = encoded;
    const auto scale = truncated.find(R"("scale_content":")" + std::string(64, 'a'));
    REQUIRE(scale != std::string::npos);
    truncated.replace(scale, std::string_view(R"("scale_content":")").size() + 64,
                      R"("scale_content":"aaaa)");
    REQUIRE_FALSE(deserialize_project(truncated, registry));

    // An unknown system name is not a system.
    auto unknown = encoded;
    const auto system = unknown.find(R"("system":"scala")");
    REQUIRE(system != std::string::npos);
    unknown.replace(system, std::string_view(R"("system":"scala")").size(),
                    R"("system":"bohlen")");
    REQUIRE_FALSE(deserialize_project(unknown, registry));
}
