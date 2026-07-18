#include <pulp/audio/sample_heritage_json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

using namespace pulp::audio;

SampleHeritageProfile complete_profile() {
    return {
        .schema_version = 1,
        .profile_id = "neutral.canonical-v1",
        .host_sample_rate = 48000.0,
        .stages = {
            {true, SampleHeritageMachineDomainStage{48000.0}},
            {true, SampleHeritageQuantizationStage{
                       12, 0.5f, UINT64_MAX,
                       SampleHeritageSeedPolicy::ContinueSerializedState}},
            {true, SampleHeritageClockPitchStage{1.0}},
            {false, SampleHeritageDacHoldStage{2}},
            {false, SampleHeritageReconstructionFilterStage{12000.0}},
            {false, SampleHeritageNoiseStage{
                        0.25f, 7,
                        SampleHeritageSeedPolicy::RestartFromProfileSeed}},
            {false, SampleHeritageOutputStage{0.75f}},
        },
    };
}

SampleHeritageProfile continuation_profile(std::string id = "neutral.state-v1") {
    return {
        .schema_version = 1,
        .profile_id = std::move(id),
        .host_sample_rate = 48000.0,
        .stages = {
            {false, SampleHeritageQuantizationStage{
                        12, 0.5f, 123,
                        SampleHeritageSeedPolicy::ContinueSerializedState}},
            {false, SampleHeritageNoiseStage{
                        0.0f, 456,
                        SampleHeritageSeedPolicy::RestartFromProfileSeed}},
        },
    };
}

std::string initial_continuation_state_json() {
    const auto validated = validate_sample_heritage_profile(continuation_profile());
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare(validated.profile));
    const auto captured = engine.capture_runtime_state();
    REQUIRE(captured.valid());
    const auto written = write_sample_heritage_runtime_state_json(captured.state);
    REQUIRE(written.valid());
    return written.json;
}

constexpr std::string_view canonical_json =
    R"({"schema_version":1,"profile_id":"neutral.canonical-v1","host_sample_rate":48000,"stages":[{"type":"machine_domain","bypass":true,"sample_rate":48000},{"type":"quantization","bypass":true,"bit_depth":12,"dither_lsb":0.5,"seed":"18446744073709551615","seed_policy":"continue_serialized_state"},{"type":"clock_pitch","bypass":true,"ratio":1},{"type":"dac_hold","bypass":false,"hold_samples":2},{"type":"reconstruction_filter","bypass":false,"cutoff_hz":12000},{"type":"noise","bypass":false,"amplitude":0.25,"seed":"7","seed_policy":"restart_from_profile_seed"},{"type":"output","bypass":false,"gain":0.75}]})";

constexpr std::string_view canonical_continuation_state_json =
    R"({"schema_version":1,"profile_schema_version":1,"profile_id":"neutral.state-v1","profile_digest_version":1,"profile_digest":"a254035078ded7b0c7f7b8cd7af2dda0801b4843b7c24bffc01054bbaf9e1b1e","rng_states":[{"stage_index":0,"stage_type":"quantization","random_state":"123"}]})";

void require_error(std::string_view json,
                   SampleHeritageJsonStatus status,
                   std::string_view path) {
    if (json.find("\"rng_states\"") != std::string_view::npos) {
        const auto result = parse_sample_heritage_runtime_state_json(json);
        CAPTURE(json, result.field_path);
        REQUIRE(result.status == status);
        REQUIRE(result.field_path == path);
        return;
    }
    const auto result = parse_sample_heritage_profile_json(json);
    CAPTURE(json, result.field_path);
    REQUIRE(result.status == status);
    REQUIRE(result.field_path == path);
}

std::string replace_once(std::string source,
                         std::string_view from,
                         std::string_view to) {
    const auto position = source.find(from);
    REQUIRE(position != std::string::npos);
    source.replace(position, from.size(), to);
    return source;
}

}  // namespace

TEST_CASE("Heritage JSON schema v1 has one deterministic canonical representation",
          "[audio][sampler][heritage][json]") {
    const auto written = write_sample_heritage_profile_json(complete_profile());
    REQUIRE(written.valid());
    REQUIRE(written.profile_status == SampleHeritageProfileStatus::Ok);
    REQUIRE(written.json == canonical_json);

    const auto parsed = parse_sample_heritage_profile_json(written.json);
    REQUIRE(parsed.valid());
    REQUIRE(parsed.profile_status == SampleHeritageProfileStatus::Ok);
    REQUIRE(parsed.profile.profile_id == "neutral.canonical-v1");
    REQUIRE(parsed.profile.host_sample_rate == 48000.0);
    REQUIRE(parsed.profile.stages.size() == 7);
    const auto& quantization = std::get<SampleHeritageQuantizationStage>(
        parsed.profile.stages[1].parameters);
    REQUIRE(quantization.seed == UINT64_MAX);
    REQUIRE(quantization.seed_policy ==
            SampleHeritageSeedPolicy::ContinueSerializedState);
    REQUIRE(std::get<SampleHeritageNoiseStage>(parsed.profile.stages[5].parameters)
                .amplitude == 0.25f);
    REQUIRE(write_sample_heritage_profile_json(parsed.profile).json == canonical_json);
}

TEST_CASE("Heritage JSON accepts insignificant whitespace but rewrites canonical form",
          "[audio][sampler][heritage][json]") {
    const auto spaced = replace_once(std::string(canonical_json),
                                     R"({"schema_version":1,)",
                                     "{ \n  \"schema_version\" : 1 , ");
    const auto parsed = parse_sample_heritage_profile_json(spaced);
    REQUIRE(parsed.valid());
    REQUIRE(write_sample_heritage_profile_json(parsed.profile).json == canonical_json);
}

TEST_CASE("Heritage identity canonicalizes signed zero across JSON and state restore",
          "[audio][sampler][heritage][json][state]") {
    auto profile = continuation_profile("neutral.signed-zero");
    std::get<SampleHeritageQuantizationStage>(profile.stages[0].parameters)
        .dither_lsb = -0.0f;
    std::get<SampleHeritageNoiseStage>(profile.stages[1].parameters).amplitude =
        -0.0f;
    profile.stages.push_back({false, SampleHeritageOutputStage{-0.0f}});

    const auto original = validate_sample_heritage_profile(profile);
    REQUIRE(original.valid());
    REQUIRE_FALSE(std::signbit(
        std::get<SampleHeritageQuantizationStage>(
            original.profile.stages[0].parameters).dither_lsb));
    REQUIRE_FALSE(std::signbit(
        std::get<SampleHeritageNoiseStage>(
            original.profile.stages[1].parameters).amplitude));
    REQUIRE_FALSE(std::signbit(
        std::get<SampleHeritageOutputStage>(
            original.profile.stages[2].parameters).gain));

    const auto written = write_sample_heritage_profile_json(profile);
    REQUIRE(written.valid());
    const auto parsed = parse_sample_heritage_profile_json(written.json);
    REQUIRE(parsed.valid());
    const auto round_tripped = validate_sample_heritage_profile(parsed.profile);
    REQUIRE(round_tripped.valid());
    REQUIRE(round_tripped.profile.profile_digest ==
            original.profile.profile_digest);

    SampleHeritageEngine source;
    REQUIRE(source.prepare(original.profile));
    const auto state = source.capture_runtime_state();
    REQUIRE(state.valid());
    SampleHeritageEngine restored;
    REQUIRE(restored.prepare(round_tripped.profile));
    REQUIRE(restored.restore_runtime_state(state.state) ==
            SampleHeritageRuntimeStateStatus::Ok);
}

TEST_CASE("Heritage JSON rejects malformed roots and incomplete top-level contracts",
          "[audio][sampler][heritage][json][reject]") {
    require_error("{", SampleHeritageJsonStatus::InvalidJson, "$");
    require_error("[]", SampleHeritageJsonStatus::RootNotObject, "$");
    require_error(
        R"({"schema_version":1,"profile_id":"neutral.a","host_sample_rate":48000,"stages":[],"extra":0})",
        SampleHeritageJsonStatus::UnknownField, "$.extra");
    require_error(
        R"({"schema_version":1,"schema_version":1,"profile_id":"neutral.a","host_sample_rate":48000,"stages":[]})",
        SampleHeritageJsonStatus::DuplicateField, "$");

    constexpr std::array required{
        std::pair{"\"schema_version\":1,", "$.schema_version"},
        std::pair{"\"profile_id\":\"neutral.canonical-v1\",", "$.profile_id"},
        std::pair{"\"host_sample_rate\":48000,", "$.host_sample_rate"},
        std::pair{",\"stages\":[", "$.stages"},
    };
    for (const auto& [fragment, path] : required) {
        auto json = std::string(canonical_json);
        const auto position = json.find(fragment);
        REQUIRE(position != std::string::npos);
        if (std::string_view(fragment) == ",\"stages\":[") {
            json.erase(position, json.size() - position - 1);
        } else {
            json.erase(position, std::string_view(fragment).size());
        }
        require_error(json, SampleHeritageJsonStatus::MissingField, path);
    }
}

TEST_CASE("Heritage JSON enforces top-level field types versions and neutral IDs",
          "[audio][sampler][heritage][json][reject]") {
    require_error(replace_once(std::string(canonical_json),
                               "\"schema_version\":1", "\"schema_version\":1.0"),
                  SampleHeritageJsonStatus::WrongType, "$.schema_version");
    require_error(replace_once(std::string(canonical_json),
                               "\"schema_version\":1", "\"schema_version\":2"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.schema_version");
    require_error(replace_once(std::string(canonical_json),
                               "\"schema_version\":1", "\"schema_version\":0"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.schema_version");
    require_error(replace_once(std::string(canonical_json),
                               "\"profile_id\":\"neutral.canonical-v1\"",
                               "\"profile_id\":7"),
                  SampleHeritageJsonStatus::WrongType, "$.profile_id");
    auto branded = replace_once(std::string(canonical_json), "neutral.canonical-v1",
                                "brand.secret-model");
    require_error(branded, SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.profile_id");
    require_error(replace_once(std::string(canonical_json),
                               "\"host_sample_rate\":48000",
                               "\"host_sample_rate\":\"48000\""),
                  SampleHeritageJsonStatus::WrongType, "$.host_sample_rate");
    require_error(replace_once(std::string(canonical_json),
                               "\"host_sample_rate\":48000",
                               "\"host_sample_rate\":7999"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.host_sample_rate");
    require_error(
        R"({"schema_version":1,"profile_id":"neutral.a","host_sample_rate":48000,"stages":null})",
                  SampleHeritageJsonStatus::WrongType, "$.stages");
}

TEST_CASE("Heritage JSON rejects unknown duplicate and missing stage fields",
          "[audio][sampler][heritage][json][reject]") {
    require_error(replace_once(std::string(canonical_json),
                               "\"gain\":0.75}", "\"gain\":0.75,\"x\":0}"),
                  SampleHeritageJsonStatus::UnknownField, "$.stages[6].x");
    require_error(replace_once(std::string(canonical_json),
                               "\"gain\":0.75}",
                               "\"gain\":0.75,\"gain\":1}"),
                  SampleHeritageJsonStatus::DuplicateField, "$");
    require_error(replace_once(std::string(canonical_json),
                               "\"gain\":0.75", "\"other\":0.75"),
                  SampleHeritageJsonStatus::UnknownField, "$.stages[6].other");
    require_error(replace_once(std::string(canonical_json),
                               "\"type\":\"output\",", ""),
                  SampleHeritageJsonStatus::MissingField, "$.stages[6].type");
    require_error(replace_once(std::string(canonical_json),
                               "\"bypass\":false,\"gain\"",
                               "\"gain\""),
                  SampleHeritageJsonStatus::MissingField, "$.stages[6].bypass");
    require_error(replace_once(std::string(canonical_json),
                               "\"gain\":0.75", "\"gain\":false"),
                  SampleHeritageJsonStatus::WrongType, "$.stages[6].gain");
}

TEST_CASE("Heritage JSON requires every parameter in every typed stage contract",
          "[audio][sampler][heritage][json][reject]") {
    constexpr std::array cases{
        std::pair{R"({"type":"machine_domain","bypass":true})",
                  "$.stages[0].sample_rate"},
        std::pair{R"({"type":"quantization","bypass":true,"dither_lsb":0,"seed":"0","seed_policy":"restart_from_profile_seed"})",
                  "$.stages[0].bit_depth"},
        std::pair{R"({"type":"quantization","bypass":true,"bit_depth":12,"seed":"0","seed_policy":"restart_from_profile_seed"})",
                  "$.stages[0].dither_lsb"},
        std::pair{R"({"type":"quantization","bypass":true,"bit_depth":12,"dither_lsb":0,"seed_policy":"restart_from_profile_seed"})",
                  "$.stages[0].seed"},
        std::pair{R"({"type":"quantization","bypass":true,"bit_depth":12,"dither_lsb":0,"seed":"0"})",
                  "$.stages[0].seed_policy"},
        std::pair{R"({"type":"clock_pitch","bypass":true})",
                  "$.stages[0].ratio"},
        std::pair{R"({"type":"dac_hold","bypass":true})",
                  "$.stages[0].hold_samples"},
        std::pair{R"({"type":"reconstruction_filter","bypass":true})",
                  "$.stages[0].cutoff_hz"},
        std::pair{R"({"type":"noise","bypass":true,"seed":"0","seed_policy":"restart_from_profile_seed"})",
                  "$.stages[0].amplitude"},
        std::pair{R"({"type":"noise","bypass":true,"amplitude":0,"seed_policy":"restart_from_profile_seed"})",
                  "$.stages[0].seed"},
        std::pair{R"({"type":"noise","bypass":true,"amplitude":0,"seed":"0"})",
                  "$.stages[0].seed_policy"},
        std::pair{R"({"type":"output","bypass":true})", "$.stages[0].gain"},
    };
    for (const auto& [stage, path] : cases) {
        const auto json =
            std::string(R"({"schema_version":1,"profile_id":"neutral.a","host_sample_rate":48000,"stages":[)") +
            stage + "]}";
        require_error(json, SampleHeritageJsonStatus::MissingField, path);
    }

    require_error(
        R"({"schema_version":1,"profile_id":"neutral.a","host_sample_rate":48000,"stages":[1]})",
        SampleHeritageJsonStatus::WrongType, "$.stages[0]");
    constexpr std::string_view output_stage =
        R"({"type":"output","bypass":true,"gain":1})";
    std::string too_many =
        R"({"schema_version":1,"profile_id":"neutral.a","host_sample_rate":48000,"stages":[)";
    for (int index = 0; index < 8; ++index) {
        if (index != 0) too_many.push_back(',');
        too_many += output_stage;
    }
    too_many += "]}";
    require_error(too_many, SampleHeritageJsonStatus::NumberOutOfRange, "$.stages");
}

TEST_CASE("Heritage JSON enforces every stage discriminant and scalar category",
          "[audio][sampler][heritage][json][reject]") {
    require_error(replace_once(std::string(canonical_json),
                               "\"type\":\"machine_domain\"",
                               "\"type\":\"unknown\""),
                  SampleHeritageJsonStatus::InvalidEnum, "$.stages[0].type");
    require_error(replace_once(std::string(canonical_json),
                               "\"type\":\"machine_domain\"",
                               "\"type\":2"),
                  SampleHeritageJsonStatus::WrongType, "$.stages[0].type");
    require_error(replace_once(std::string(canonical_json),
                               "\"bypass\":true", "\"bypass\":1"),
                  SampleHeritageJsonStatus::WrongType, "$.stages[0].bypass");
    require_error(replace_once(std::string(canonical_json),
                               "\"sample_rate\":48000", "\"sample_rate\":0"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.stages[0].sample_rate");
    require_error(replace_once(std::string(canonical_json),
                               "\"bit_depth\":12", "\"bit_depth\":12.0"),
                  SampleHeritageJsonStatus::WrongType, "$.stages[1].bit_depth");
    require_error(replace_once(std::string(canonical_json),
                               "\"bit_depth\":12", "\"bit_depth\":25"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.stages[1].bit_depth");
    require_error(replace_once(std::string(canonical_json),
                               "\"dither_lsb\":0.5", "\"dither_lsb\":3"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.stages[1].dither_lsb");
    require_error(replace_once(std::string(canonical_json),
                               "\"ratio\":1", "\"ratio\":false"),
                  SampleHeritageJsonStatus::WrongType, "$.stages[2].ratio");
    require_error(replace_once(std::string(canonical_json),
                               "\"hold_samples\":2", "\"hold_samples\":0"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.stages[3].hold_samples");
    require_error(replace_once(std::string(canonical_json),
                               "\"cutoff_hz\":12000", "\"cutoff_hz\":24000"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.stages[4]");
    require_error(replace_once(std::string(canonical_json),
                               "\"amplitude\":0.25", "\"amplitude\":1.1"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.stages[5].amplitude");
    require_error(replace_once(std::string(canonical_json),
                               "\"gain\":0.75", "\"gain\":17"),
                  SampleHeritageJsonStatus::NumberOutOfRange, "$.stages[6].gain");
}

TEST_CASE("Heritage JSON preserves uint64 seeds and rejects noncanonical seed text",
          "[audio][sampler][heritage][json][reject]") {
    for (const auto invalid : {"7", "\"\"", "\"07\"", "\"-1\"",
                               "\"18446744073709551616\"", "\"7x\""}) {
        require_error(replace_once(std::string(canonical_json),
                                   "\"18446744073709551615\"", invalid),
                      invalid == std::string_view("7")
                          ? SampleHeritageJsonStatus::WrongType
                          : SampleHeritageJsonStatus::NumberOutOfRange,
                      "$.stages[1].seed");
    }
    require_error(replace_once(std::string(canonical_json),
                               "continue_serialized_state", "future_policy"),
                  SampleHeritageJsonStatus::InvalidEnum,
                  "$.stages[1].seed_policy");
    require_error(replace_once(std::string(canonical_json),
                               "\"seed_policy\":\"continue_serialized_state\"",
                               "\"seed_policy\":false"),
                  SampleHeritageJsonStatus::WrongType,
                  "$.stages[1].seed_policy");
}

TEST_CASE("Heritage JSON delegates cross-field and duplicate-stage semantics",
          "[audio][sampler][heritage][json][reject]") {
    require_error(replace_once(std::string(canonical_json),
                               "\"bypass\":true,\"sample_rate\":48000",
                               "\"bypass\":false,\"sample_rate\":44100"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.stages[0]");
    require_error(replace_once(std::string(canonical_json),
                               "\"bypass\":true,\"ratio\":1",
                               "\"bypass\":false,\"ratio\":0.5"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.stages[2]");
    require_error(replace_once(std::string(canonical_json),
                               R"({"type":"output","bypass":false,"gain":0.75})",
                               R"({"type":"noise","bypass":false,"amplitude":0.5,"seed":"9","seed_policy":"restart_from_profile_seed"})"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.stages[6]");
}

TEST_CASE("Heritage JSON writer refuses invalid authoring profiles",
          "[audio][sampler][heritage][json][reject]") {
    auto invalid = complete_profile();
    invalid.profile_id = "proprietary.model";
    const auto written = write_sample_heritage_profile_json(invalid);
    REQUIRE_FALSE(written.valid());
    REQUIRE(written.json.empty());
    REQUIRE(written.profile_status == SampleHeritageProfileStatus::InvalidProfileId);

    invalid = complete_profile();
    std::get<SampleHeritageNoiseStage>(invalid.stages[5].parameters).seed_policy =
        static_cast<SampleHeritageSeedPolicy>(255);
    const auto invalid_policy = write_sample_heritage_profile_json(invalid);
    REQUIRE_FALSE(invalid_policy.valid());
    REQUIRE(invalid_policy.json.empty());
    REQUIRE(invalid_policy.profile_status ==
            SampleHeritageProfileStatus::InvalidStageParameter);
}

TEST_CASE("Heritage continuation state has one profile-bound canonical JSON form",
          "[audio][sampler][heritage][json][state]") {
    const auto validated =
        validate_sample_heritage_profile(continuation_profile());
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare(validated.profile));
    const auto captured = engine.capture_runtime_state();
    REQUIRE(captured.valid());
    REQUIRE(captured.state.bound_profile_id() == "neutral.state-v1");
    REQUIRE(captured.state.rng_state_count == 1);
    REQUIRE(captured.state.rng_states[0].stage_index == 0);
    REQUIRE(captured.state.rng_states[0].stage_type ==
            SampleHeritageRuntimeRngStageType::Quantization);
    REQUIRE(captured.state.rng_states[0].random_state == 123);

    const auto written =
        write_sample_heritage_runtime_state_json(captured.state);
    REQUIRE(written.valid());
    REQUIRE(written.json == canonical_continuation_state_json);
    REQUIRE(initial_continuation_state_json() == canonical_continuation_state_json);
    const auto parsed =
        parse_sample_heritage_runtime_state_json(written.json);
    REQUIRE(parsed.valid());
    REQUIRE(parsed.state.bound_profile_id() == "neutral.state-v1");
    REQUIRE(parsed.state.rng_state_count == 1);
    REQUIRE(parsed.state.rng_states[0].random_state == 123);
    REQUIRE(write_sample_heritage_runtime_state_json(parsed.state).json ==
            written.json);
}

TEST_CASE("Heritage continuation restores opted-in RNG and restarts restart-policy RNG",
          "[audio][sampler][heritage][json][state]") {
    const auto validated =
        validate_sample_heritage_profile(continuation_profile());
    REQUIRE(validated.valid());
    SampleHeritageEngine source;
    REQUIRE(source.prepare(validated.profile));
    Buffer<float> prefix(1, 19);
    REQUIRE(source.process(prefix.view()));
    const auto captured = source.capture_runtime_state();
    REQUIRE(captured.valid());
    REQUIRE(captured.state.rng_states[0].random_state != 123);

    Buffer<float> expected(1, 31);
    REQUIRE(source.process(expected.view()));

    const auto json = write_sample_heritage_runtime_state_json(captured.state);
    REQUIRE(json.valid());
    const auto parsed = parse_sample_heritage_runtime_state_json(json.json);
    REQUIRE(parsed.valid());
    SampleHeritageEngine restored;
    REQUIRE(restored.prepare(validated.profile));
    REQUIRE(restored.restore_runtime_state(parsed.state) ==
            SampleHeritageRuntimeStateStatus::Ok);
    const auto resumed = restored.capture_runtime_state();
    REQUIRE(resumed.valid());
    REQUIRE(resumed.state.rng_states[0].random_state ==
            captured.state.rng_states[0].random_state);

    Buffer<float> actual(1, 31);
    REQUIRE(restored.process(actual.view()));
    for (std::size_t frame = 0; frame < actual.num_samples(); ++frame)
        REQUIRE(actual.channel(0)[frame] == expected.channel(0)[frame]);
}

TEST_CASE("Heritage continuation resumes RNG but resets hold and filter transients",
          "[audio][sampler][heritage][json][state]") {
    SampleHeritageProfile profile{
        .schema_version = 1,
        .profile_id = "neutral.rng-only-continuation",
        .host_sample_rate = 48000.0,
        .stages = {
            {false, SampleHeritageNoiseStage{
                        0.125f, 987,
                        SampleHeritageSeedPolicy::ContinueSerializedState}},
            {false, SampleHeritageDacHoldStage{3}},
            {false, SampleHeritageReconstructionFilterStage{7000.0}},
        },
    };
    const auto validated = validate_sample_heritage_profile(profile);
    REQUIRE(validated.valid());

    SampleHeritageEngine uninterrupted;
    REQUIRE(uninterrupted.prepare(validated.profile));
    Buffer<float> prefix(1, 5);
    std::fill(prefix.channel(0).begin(), prefix.channel(0).end(), 0.2f);
    REQUIRE(uninterrupted.process(prefix.view()));
    const auto state = uninterrupted.capture_runtime_state();
    REQUIRE(state.valid());

    Buffer<float> uninterrupted_output(1, 17);
    for (std::size_t frame = 0; frame < uninterrupted_output.num_samples(); ++frame)
        uninterrupted_output.channel(0)[frame] =
            static_cast<float>(frame) * 0.01f - 0.08f;
    Buffer<float> restored_output = uninterrupted_output;
    REQUIRE(uninterrupted.process(uninterrupted_output.view()));

    SampleHeritageEngine restored;
    REQUIRE(restored.prepare(validated.profile));
    REQUIRE(restored.restore_runtime_state(state.state) ==
            SampleHeritageRuntimeStateStatus::Ok);
    REQUIRE(restored.process(restored_output.view()));

    bool transient_outputs_differ = false;
    for (std::size_t frame = 0; frame < restored_output.num_samples(); ++frame) {
        if (restored_output.channel(0)[frame] !=
            uninterrupted_output.channel(0)[frame]) {
            transient_outputs_differ = true;
            break;
        }
    }
    REQUIRE(transient_outputs_differ);

    const auto uninterrupted_rng = uninterrupted.capture_runtime_state();
    const auto restored_rng = restored.capture_runtime_state();
    REQUIRE(uninterrupted_rng.valid());
    REQUIRE(restored_rng.valid());
    REQUIRE(uninterrupted_rng.state.rng_states[0].random_state ==
            restored_rng.state.rng_states[0].random_state);
}

TEST_CASE("Heritage continuation rejects profile and stage-layout mismatches atomically",
          "[audio][sampler][heritage][json][state][reject]") {
    const auto validated =
        validate_sample_heritage_profile(continuation_profile());
    REQUIRE(validated.valid());
    SampleHeritageEngine engine;
    REQUIRE(engine.prepare(validated.profile));
    auto state = engine.capture_runtime_state().state;

    auto wrong_id = state;
    wrong_id.profile_id = {};
    constexpr std::string_view other_id = "neutral.other-v1";
    std::copy(other_id.begin(), other_id.end(), wrong_id.profile_id.begin());
    REQUIRE(engine.restore_runtime_state(wrong_id) ==
            SampleHeritageRuntimeStateStatus::ProfileMismatch);
    REQUIRE(engine.capture_runtime_state().state.rng_states[0].random_state == 123);

    auto wrong_profile_version = state;
    wrong_profile_version.profile_schema_version = 2;
    REQUIRE(engine.restore_runtime_state(wrong_profile_version) ==
            SampleHeritageRuntimeStateStatus::ProfileMismatch);
    auto wrong_state_version = state;
    wrong_state_version.schema_version = 2;
    REQUIRE(engine.restore_runtime_state(wrong_state_version) ==
            SampleHeritageRuntimeStateStatus::UnsupportedSchemaVersion);
    auto wrong_index = state;
    wrong_index.rng_states[0].stage_index = 1;
    REQUIRE(engine.restore_runtime_state(wrong_index) ==
            SampleHeritageRuntimeStateStatus::InvalidStageLayout);
    auto wrong_type = state;
    wrong_type.rng_states[0].stage_type =
        SampleHeritageRuntimeRngStageType::Noise;
    REQUIRE(engine.restore_runtime_state(wrong_type) ==
            SampleHeritageRuntimeStateStatus::InvalidStageLayout);
    auto zero_state = state;
    zero_state.rng_states[0].random_state = 0;
    REQUIRE(engine.restore_runtime_state(zero_state) ==
            SampleHeritageRuntimeStateStatus::InvalidRandomState);
    auto missing = state;
    missing.rng_state_count = 0;
    REQUIRE(engine.restore_runtime_state(missing) ==
            SampleHeritageRuntimeStateStatus::InvalidStageLayout);
    auto extra = state;
    extra.rng_state_count = 2;
    extra.rng_states[1] = {1, SampleHeritageRuntimeRngStageType::Noise, 9};
    REQUIRE(engine.restore_runtime_state(extra) ==
            SampleHeritageRuntimeStateStatus::InvalidStageLayout);
    REQUIRE(engine.capture_runtime_state().state.rng_states[0].random_state == 123);

    auto changed_profile = continuation_profile();
    std::get<SampleHeritageQuantizationStage>(
        changed_profile.stages[0].parameters).bit_depth = 11;
    const auto changed_validated = validate_sample_heritage_profile(changed_profile);
    REQUIRE(changed_validated.valid());
    SampleHeritageEngine changed_engine;
    REQUIRE(changed_engine.prepare(changed_validated.profile));
    REQUIRE(changed_engine.restore_runtime_state(state) ==
            SampleHeritageRuntimeStateStatus::ProfileMismatch);
}

TEST_CASE("Restart-policy RNG state is never serialized and restarts on state restore",
          "[audio][sampler][heritage][json][state]") {
    auto profile = continuation_profile("neutral.restart-proof");
    profile.stages = {{false, SampleHeritageNoiseStage{
        0.25f, 456, SampleHeritageSeedPolicy::RestartFromProfileSeed}}};
    const auto validated = validate_sample_heritage_profile(profile);
    REQUIRE(validated.valid());
    SampleHeritageEngine progressed;
    REQUIRE(progressed.prepare(validated.profile));
    Buffer<float> prefix(1, 17);
    REQUIRE(progressed.process(prefix.view()));
    const auto state = progressed.capture_runtime_state();
    REQUIRE(state.valid());
    REQUIRE(state.state.rng_state_count == 0);
    REQUIRE(progressed.restore_runtime_state(state.state) ==
            SampleHeritageRuntimeStateStatus::Ok);

    SampleHeritageEngine fresh;
    REQUIRE(fresh.prepare(validated.profile));
    Buffer<float> actual(1, 23);
    Buffer<float> expected(1, 23);
    REQUIRE(progressed.process(actual.view()));
    REQUIRE(fresh.process(expected.view()));
    for (std::size_t frame = 0; frame < actual.num_samples(); ++frame)
        REQUIRE(actual.channel(0)[frame] == expected.channel(0)[frame]);
}

TEST_CASE("Heritage continuation JSON strictly rejects malformed state contracts",
          "[audio][sampler][heritage][json][state][reject]") {
    const auto valid = initial_continuation_state_json();
    require_error(replace_once(std::string(valid),
                               "\"schema_version\":1,",
                               "\"schema_version\":1,\"schema_version\":1,"),
                  SampleHeritageJsonStatus::DuplicateField, "$");
    require_error(replace_once(std::string(valid),
                               "\"profile_id\":\"neutral.state-v1\",",
                               "\"profile_id\":\"neutral.state-v1\",\"x\":0,"),
                  SampleHeritageJsonStatus::UnknownField, "$.x");
    require_error(replace_once(std::string(valid),
                               "\"profile_schema_version\":1,", ""),
                  SampleHeritageJsonStatus::MissingField,
                  "$.profile_schema_version");
    require_error(replace_once(std::string(valid),
                               "\"profile_digest_version\":1,", ""),
                  SampleHeritageJsonStatus::MissingField,
                  "$.profile_digest_version");
    require_error(replace_once(
                      std::string(valid),
                      "\"profile_digest\":\"a254035078ded7b0c7f7b8cd7af2dda0801b4843b7c24bffc01054bbaf9e1b1e\",",
                      ""),
                  SampleHeritageJsonStatus::MissingField,
                  "$.profile_digest");
    require_error(replace_once(std::string(valid),
                               "\"schema_version\":1",
                               "\"schema_version\":2"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.schema_version");
    require_error(replace_once(std::string(valid),
                               "\"profile_schema_version\":1",
                               "\"profile_schema_version\":2"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.profile_schema_version");
    require_error(replace_once(std::string(valid),
                               "\"profile_digest_version\":1",
                               "\"profile_digest_version\":2"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.profile_digest_version");
    require_error(replace_once(std::string(valid),
                               "a2540350", "A2540350"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.profile_digest");
    require_error(replace_once(std::string(valid),
                               "a2540350", "a254035"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.profile_digest");
    require_error(replace_once(std::string(valid), "neutral.state-v1", "brand.x"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.profile_id");
    require_error(
        replace_once(initial_continuation_state_json(),
                     "\"rng_states\":[{\"stage_index\":0,\"stage_type\":\"quantization\",\"random_state\":\"123\"}]",
                     "\"rng_states\":false"),
        SampleHeritageJsonStatus::WrongType, "$.rng_states");
    require_error(replace_once(std::string(valid),
                               "\"stage_index\":0", "\"stage_index\":0.0"),
                  SampleHeritageJsonStatus::WrongType,
                  "$.rng_states[0].stage_index");
    require_error(replace_once(std::string(valid),
                               "\"stage_index\":0", "\"stage_index\":7"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.rng_states[0].stage_index");
    require_error(replace_once(std::string(valid), "quantization", "future"),
                  SampleHeritageJsonStatus::InvalidEnum,
                  "$.rng_states[0].stage_type");
    require_error(replace_once(std::string(valid),
                               "\"random_state\":\"123\"",
                               "\"random_state\":\"123\",\"extra\":0"),
                  SampleHeritageJsonStatus::UnknownField,
                  "$.rng_states[0].extra");
    require_error(replace_once(std::string(valid),
                               "\"stage_type\":\"quantization\",", ""),
                  SampleHeritageJsonStatus::MissingField,
                  "$.rng_states[0].stage_type");
    require_error(replace_once(std::string(valid),
                               "\"stage_index\":0,",
                               "\"stage_index\":0,\"stage_index\":0,"),
                  SampleHeritageJsonStatus::DuplicateField, "$");
    require_error(replace_once(std::string(valid),
                               "\"random_state\":\"123\"",
                               "\"random_state\":123"),
                  SampleHeritageJsonStatus::WrongType,
                  "$.rng_states[0].random_state");
    for (const auto invalid : {"\"0\"", "\"0123\"", "\"-1\"",
                               "\"18446744073709551616\""}) {
        require_error(replace_once(std::string(valid), "\"123\"", invalid),
                      SampleHeritageJsonStatus::NumberOutOfRange,
                      "$.rng_states[0].random_state");
    }
}

TEST_CASE("Heritage continuation JSON rejects duplicate and noncanonical RNG layouts",
          "[audio][sampler][heritage][json][state][reject]") {
    require_error(replace_once(
        initial_continuation_state_json(),
        R"([{"stage_index":0,"stage_type":"quantization","random_state":"123"}])",
        R"([{"stage_index":1,"stage_type":"noise","random_state":"9"},{"stage_index":0,"stage_type":"quantization","random_state":"8"}])"),
        SampleHeritageJsonStatus::ProfileValidationFailed, "$.rng_states");
    require_error(replace_once(
        initial_continuation_state_json(),
        R"([{"stage_index":0,"stage_type":"quantization","random_state":"123"}])",
        R"([{"stage_index":0,"stage_type":"quantization","random_state":"9"},{"stage_index":0,"stage_type":"noise","random_state":"8"}])"),
        SampleHeritageJsonStatus::ProfileValidationFailed, "$.rng_states");

    SampleHeritageRuntimeState forged;
    forged.schema_version = 2;
    forged.profile_schema_version = 1;
    constexpr std::string_view id = "neutral.state-v1";
    std::copy(id.begin(), id.end(), forged.profile_id.begin());
    const auto rejected = write_sample_heritage_runtime_state_json(forged);
    REQUIRE_FALSE(rejected.valid());
    REQUIRE(rejected.json.empty());
    REQUIRE(rejected.runtime_status ==
            SampleHeritageRuntimeStateStatus::UnsupportedSchemaVersion);
}
