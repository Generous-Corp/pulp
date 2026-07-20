#include <pulp/audio/sample_heritage_json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace pulp::audio;

SampleHeritageProfile continuation_profile(std::string id = "neutral.state-v1") {
    return {
        .schema_version = kSampleHeritageProfileSchemaVersion,
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

std::string digest_hex(const std::array<std::uint8_t, 32>& digest) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

std::string expected_initial_continuation_state_json() {
    const auto validated = validate_sample_heritage_profile(continuation_profile());
    REQUIRE(validated.valid());
    return "{\"schema_version\":1,\"profile_schema_version\":3,"
           "\"profile_id\":\"neutral.state-v1\","
           "\"profile_digest_version\":3,\"profile_digest\":\"" +
           digest_hex(validated.profile.profile_digest) +
           "\",\"rng_states\":[{\"stage_index\":0,"
           "\"stage_type\":\"quantization\",\"random_state\":\"123\"}]}";
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

void require_error(std::string_view json, SampleHeritageJsonStatus status,
                   std::string_view path) {
    const auto result = parse_sample_heritage_runtime_state_json(json);
    CAPTURE(json, result.field_path);
    REQUIRE(result.status == status);
    REQUIRE(result.field_path == path);
}

std::string replace_once(std::string source, std::string_view from,
                         std::string_view to) {
    const auto position = source.find(from);
    REQUIRE(position != std::string::npos);
    source.replace(position, from.size(), to);
    return source;
}

std::string uppercase_digest(std::string json) {
    const auto marker = json.find("\"profile_digest\":\"");
    REQUIRE(marker != std::string::npos);
    const auto begin = marker + std::string_view("\"profile_digest\":\"").size();
    const auto end = begin + 64;
    const auto letter = std::find_if(json.begin() + static_cast<std::ptrdiff_t>(begin),
                                     json.begin() + static_cast<std::ptrdiff_t>(end),
                                     [](char character) {
                                         return character >= 'a' && character <= 'f';
                                     });
    REQUIRE(letter != json.begin() + static_cast<std::ptrdiff_t>(end));
    *letter = static_cast<char>(std::toupper(static_cast<unsigned char>(*letter)));
    return json;
}

SampleHeritageTypedRuntimeState typed_runtime_state() {
    SampleHeritageTypedRuntimeState state;
    state.profile_schema_version = kSampleHeritageProfileSchemaVersion;
    constexpr std::string_view id = "neutral.typed-state";
    std::copy(id.begin(), id.end(), state.profile_id.begin());
    for (std::size_t index = 0; index < state.profile_digest.size(); ++index)
        state.profile_digest[index] = static_cast<std::uint8_t>(index);
    state.host_sample_rate = 44100.5;
    state.voice_states[2].engine.rng_states[0] = {
        0, SampleHeritageRuntimeRngStageType::Quantization, 11};
    state.voice_states[2].engine.rng_state_count = 1;
    state.voice_states[7].engine.rng_states[0] = {
        3, SampleHeritageRuntimeRngStageType::LiveCyclic, 22};
    state.voice_states[7].engine.rng_state_count = 1;
    state.bus_state.rng_states[0] = {
        1, SampleHeritageRuntimeRngStageType::Noise, 33};
    state.bus_state.rng_state_count = 1;
    return state;
}

std::string expected_typed_runtime_state_json() {
    return R"({"schema_version":2,"profile_schema_version":3,"profile_id":"neutral.typed-state","profile_digest_version":3,"profile_digest":"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f","host_sample_rate":44100.5,"voice_states":[{"slot_index":0,"rng_states":[]},{"slot_index":1,"rng_states":[]},{"slot_index":2,"rng_states":[{"stage_index":0,"stage_type":"quantization","random_state":"11"}]},{"slot_index":3,"rng_states":[]},{"slot_index":4,"rng_states":[]},{"slot_index":5,"rng_states":[]},{"slot_index":6,"rng_states":[]},{"slot_index":7,"rng_states":[{"stage_index":3,"stage_type":"live_cyclic","random_state":"22"}]}],"bus_state":{"rng_states":[{"stage_index":1,"stage_type":"noise","random_state":"33"}]}})";
}

}  // namespace

TEST_CASE("Heritage identity canonicalizes signed zero across profile JSON",
          "[audio][sampler][heritage][json][state]") {
    SampleHeritageProfile profile{
        .schema_version = kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.signed-zero",
        .host_sample_rate = 48000.0,
        .voice = {{
            SampleHeritageBlockDomain::Voice,
            false,
            SampleHeritageVoiceConverterBlock{
                SampleHeritageConverterFamily::LinearPcm,
                12,
                0.0f,
                -0.0f,
                123,
                SampleHeritageSeedPolicy::ContinueSerializedState},
        }},
        .bus = {
            {SampleHeritageBlockDomain::Bus,
             false,
             SampleHeritageBusNoiseIdleBlock{
                 -0.0f,
                 -0.0f,
                 -0.0f,
                 SampleHeritageNoiseGate::AlwaysOn,
                 456,
                 SampleHeritageSeedPolicy::RestartFromProfileSeed}},
            {SampleHeritageBlockDomain::Bus,
             false,
             SampleHeritageBusOutputDriveBlock{-0.0f, 1.0f}},
        },
    };

    const auto original = validate_sample_heritage_profile(profile);
    REQUIRE(original.valid());
    const auto written = write_sample_heritage_profile_json(profile);
    REQUIRE(written.valid());
    const auto parsed = parse_sample_heritage_profile_json(written.json);
    REQUIRE(parsed.valid());
    const auto round_tripped = validate_sample_heritage_profile(parsed.profile);
    REQUIRE(round_tripped.valid());
    REQUIRE(round_tripped.profile.profile_digest ==
            original.profile.profile_digest);
    REQUIRE_FALSE(std::signbit(std::get<SampleHeritageVoiceConverterBlock>(
                                   round_tripped.profile.voice[0].parameters)
                                   .dither_lsb));
    REQUIRE_FALSE(std::signbit(std::get<SampleHeritageBusNoiseIdleBlock>(
                                   round_tripped.profile.bus[0].parameters)
                                   .noise_amplitude));
    REQUIRE_FALSE(std::signbit(std::get<SampleHeritageBusOutputDriveBlock>(
                                   round_tripped.profile.bus[1].parameters)
                                   .drive));

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
    REQUIRE(captured.state.profile_schema_version == 3);
    REQUIRE(captured.state.profile_digest_version == 3);
    REQUIRE(captured.state.bound_profile_id() == "neutral.state-v1");
    REQUIRE(captured.state.rng_state_count == 1);
    REQUIRE(captured.state.rng_states[0].stage_index == 0);
    REQUIRE(captured.state.rng_states[0].stage_type ==
            SampleHeritageRuntimeRngStageType::Quantization);
    REQUIRE(captured.state.rng_states[0].random_state == 123);

    const auto written = write_sample_heritage_runtime_state_json(captured.state);
    REQUIRE(written.valid());
    REQUIRE(written.json == expected_initial_continuation_state_json());
    REQUIRE(initial_continuation_state_json() ==
            expected_initial_continuation_state_json());
    const auto parsed = parse_sample_heritage_runtime_state_json(written.json);
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
        .schema_version = kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.rng-only-continuation",
        .host_sample_rate = 48000.0,
        .stages = {
            {false, SampleHeritageDacHoldStage{3}},
            {false, SampleHeritageReconstructionFilterStage{7000.0}},
            {false, SampleHeritageNoiseStage{
                        0.125f, 987,
                        SampleHeritageSeedPolicy::ContinueSerializedState}},
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
    wrong_profile_version.profile_schema_version = 4;
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
                               "\"profile_schema_version\":3,", ""),
                  SampleHeritageJsonStatus::MissingField,
                  "$.profile_schema_version");
    require_error(replace_once(std::string(valid),
                               "\"profile_digest_version\":3,", ""),
                  SampleHeritageJsonStatus::MissingField,
                  "$.profile_digest_version");
    const auto digest = digest_hex(
        validate_sample_heritage_profile(continuation_profile())
            .profile.profile_digest);
    require_error(replace_once(std::string(valid),
                               "\"profile_digest\":\"" + digest + "\",", ""),
                  SampleHeritageJsonStatus::MissingField,
                  "$.profile_digest");
    require_error(replace_once(std::string(valid),
                               "\"schema_version\":1",
                               "\"schema_version\":2"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.schema_version");
    require_error(replace_once(std::string(valid),
                               "\"profile_schema_version\":3",
                               "\"profile_schema_version\":4"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.profile_schema_version");
    require_error(replace_once(std::string(valid),
                               "\"profile_digest_version\":3",
                               "\"profile_digest_version\":4"),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.profile_digest_version");
    require_error(uppercase_digest(std::string(valid)),
                  SampleHeritageJsonStatus::ProfileValidationFailed,
                  "$.profile_digest");
    require_error(replace_once(std::string(valid), digest,
                               digest.substr(0, digest.size() - 1)),
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
    forged.profile_schema_version = kSampleHeritageProfileSchemaVersion;
    constexpr std::string_view id = "neutral.state-v1";
    std::copy(id.begin(), id.end(), forged.profile_id.begin());
    const auto rejected = write_sample_heritage_runtime_state_json(forged);
    REQUIRE_FALSE(rejected.valid());
    REQUIRE(rejected.json.empty());
    REQUIRE(rejected.runtime_status ==
            SampleHeritageRuntimeStateStatus::UnsupportedSchemaVersion);
}

TEST_CASE("Typed Heritage runtime state has one fixed-slot canonical form",
          "[audio][sampler][heritage][json][state]") {
    const auto state = typed_runtime_state();
    for (std::size_t index = 0; index < state.voice_states.size(); ++index)
        REQUIRE(state.voice_states[index].slot_index == index);

    const auto written = write_sample_heritage_typed_runtime_state_json(state);
    REQUIRE(written.valid());
    REQUIRE(written.json == expected_typed_runtime_state_json());

    const auto parsed =
        parse_sample_heritage_typed_runtime_state_json(written.json);
    REQUIRE(parsed.valid());
    REQUIRE(parsed.runtime_status == SampleHeritageRuntimeStateStatus::Ok);
    REQUIRE(parsed.state.schema_version == 2);
    REQUIRE(parsed.state.bound_profile_id() == "neutral.typed-state");
    REQUIRE(parsed.state.host_sample_rate == 44100.5);
    REQUIRE(parsed.state.voice_states[0].engine.rng_state_count == 0);
    REQUIRE(parsed.state.voice_states[2].engine.rng_states[0].random_state == 11);
    REQUIRE(parsed.state.voice_states[7].engine.rng_states[0].random_state == 22);
    REQUIRE(parsed.state.bus_state.rng_states[0].random_state == 33);
    REQUIRE(write_sample_heritage_typed_runtime_state_json(parsed.state).json ==
            written.json);
}

TEST_CASE("Typed Heritage runtime state rejects ambiguous slot layouts",
          "[audio][sampler][heritage][json][state][reject]") {
    auto wrong_slot = typed_runtime_state();
    wrong_slot.voice_states[3].slot_index = 4;
    const auto rejected =
        write_sample_heritage_typed_runtime_state_json(wrong_slot);
    REQUIRE_FALSE(rejected.valid());
    REQUIRE(rejected.runtime_status ==
            SampleHeritageRuntimeStateStatus::InvalidSlotLayout);

    const auto valid = expected_typed_runtime_state_json();
    const auto missing = replace_once(
        valid, R"(,{"slot_index":7,"rng_states":[{"stage_index":3,"stage_type":"live_cyclic","random_state":"22"}]})",
        "");
    const auto missing_result =
        parse_sample_heritage_typed_runtime_state_json(missing);
    REQUIRE_FALSE(missing_result.valid());
    REQUIRE(missing_result.status ==
            SampleHeritageJsonStatus::NumberOutOfRange);
    REQUIRE(missing_result.field_path == "$.voice_states");

    const auto duplicate_identity = replace_once(
        valid, R"("slot_index":3)", R"("slot_index":2)");
    const auto duplicate_result =
        parse_sample_heritage_typed_runtime_state_json(duplicate_identity);
    REQUIRE_FALSE(duplicate_result.valid());
    REQUIRE(duplicate_result.status ==
            SampleHeritageJsonStatus::ProfileValidationFailed);
    REQUIRE(duplicate_result.runtime_status ==
            SampleHeritageRuntimeStateStatus::InvalidSlotLayout);
    REQUIRE(duplicate_result.field_path == "$.voice_states[3].slot_index");
}

TEST_CASE("Typed Heritage runtime state rejects incompatible envelopes",
          "[audio][sampler][heritage][json][state][reject]") {
    const auto valid = expected_typed_runtime_state_json();
    const auto future = parse_sample_heritage_typed_runtime_state_json(
        replace_once(valid, R"("schema_version":2)",
                     R"("schema_version":3)"));
    REQUIRE_FALSE(future.valid());
    REQUIRE(future.runtime_status ==
            SampleHeritageRuntimeStateStatus::UnsupportedSchemaVersion);
    REQUIRE(future.field_path == "$.schema_version");

    const auto old_engine_envelope =
        parse_sample_heritage_typed_runtime_state_json(
            initial_continuation_state_json());
    REQUIRE_FALSE(old_engine_envelope.valid());
    REQUIRE(old_engine_envelope.runtime_status ==
            SampleHeritageRuntimeStateStatus::UnsupportedSchemaVersion);

    const auto wrong_rate = parse_sample_heritage_typed_runtime_state_json(
        replace_once(valid, R"("host_sample_rate":44100.5)",
                     R"("host_sample_rate":7999)"));
    REQUIRE_FALSE(wrong_rate.valid());
    REQUIRE(wrong_rate.runtime_status ==
            SampleHeritageRuntimeStateStatus::InvalidHostSampleRate);
    REQUIRE(wrong_rate.field_path == "$.host_sample_rate");

    auto nonfinite = typed_runtime_state();
    nonfinite.host_sample_rate = std::numeric_limits<double>::quiet_NaN();
    const auto nonfinite_write =
        write_sample_heritage_typed_runtime_state_json(nonfinite);
    REQUIRE_FALSE(nonfinite_write.valid());
    REQUIRE(nonfinite_write.runtime_status ==
            SampleHeritageRuntimeStateStatus::InvalidHostSampleRate);

    const auto wrong_profile = parse_sample_heritage_typed_runtime_state_json(
        replace_once(valid, R"("profile_schema_version":3)",
                     R"("profile_schema_version":4)"));
    REQUIRE_FALSE(wrong_profile.valid());
    REQUIRE(wrong_profile.runtime_status ==
            SampleHeritageRuntimeStateStatus::ProfileMismatch);
}

TEST_CASE("Typed Heritage runtime state validates each bounded engine record",
          "[audio][sampler][heritage][json][state][reject]") {
    auto invalid = typed_runtime_state();
    invalid.voice_states[2].engine.rng_states[0].random_state = 0;
    const auto invalid_write =
        write_sample_heritage_typed_runtime_state_json(invalid);
    REQUIRE_FALSE(invalid_write.valid());
    REQUIRE(invalid_write.runtime_status ==
            SampleHeritageRuntimeStateStatus::InvalidRandomState);

    const auto valid = expected_typed_runtime_state_json();
    const auto unordered = parse_sample_heritage_typed_runtime_state_json(
        replace_once(
            valid,
            R"({"stage_index":0,"stage_type":"quantization","random_state":"11"})",
            R"({"stage_index":2,"stage_type":"noise","random_state":"11"},{"stage_index":1,"stage_type":"quantization","random_state":"12"})"));
    REQUIRE_FALSE(unordered.valid());
    REQUIRE(unordered.runtime_status ==
            SampleHeritageRuntimeStateStatus::InvalidStageLayout);
    REQUIRE(unordered.field_path == "$.voice_states[2].rng_states");

    const auto unknown = parse_sample_heritage_typed_runtime_state_json(
        replace_once(valid, R"("bus_state":{"rng_states":)",
                     R"("bus_state":{"extra":0,"rng_states":)"));
    REQUIRE_FALSE(unknown.valid());
    REQUIRE(unknown.status == SampleHeritageJsonStatus::UnknownField);
    REQUIRE(unknown.field_path == "$.bus_state.extra");
}
