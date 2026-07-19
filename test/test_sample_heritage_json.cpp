#include <pulp/audio/sample_heritage_json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace pulp::audio;

namespace {

constexpr std::string_view canonical_json =
    R"({"schema_version":3,"profile_id":"neutral.canonical-v3","host_sample_rate":48000,"voice":[{"domain":"voice","type":"machine_domain","bypass":false,"sample_rate":32000},{"domain":"voice","type":"clock","bypass":false,"ratio":0.75},{"domain":"voice","type":"pitch","bypass":false,"family":"drop_repeat"},{"domain":"voice","type":"converter","bypass":false,"family":"mu_law","bit_depth":12,"dac_nonlinearity":0.1,"dither_lsb":0.5,"seed":"7","seed_policy":"continue_serialized_state"},{"domain":"voice","type":"live_cyclic_stretch","bypass":false,"factor":1.25,"cycle_ms":40,"splice_ms":2,"stereo_link":true,"tempo_lock":true,"shuffle_divisions":4,"seed":"9","seed_policy":"restart_from_profile_seed"},{"domain":"voice","type":"hold_droop","bypass":false,"mode":"zero_order","hold_samples":2,"droop":0.25},{"domain":"voice","type":"reconstruction","bypass":false,"family":"elliptic","cutoff_law":"machine_rate_ratio","cutoff_value":0.4,"order":4,"ripple_db":0.5,"stopband_attenuation_db":60},{"domain":"voice","type":"analog_color","bypass":false,"drive":1.5,"asymmetry":-0.25,"mix":0.75}],"bus":[{"domain":"bus","type":"noise_idle","bypass":false,"noise_amplitude":0.01,"idle_amplitude":0.02,"tilt_db_per_octave":-3,"tilt_reference_hz":1000,"tilt_floor_hz":20,"gate":"voice_active","seed":"11","seed_policy":"continue_serialized_state"},{"domain":"bus","type":"output_drive","bypass":false,"drive":2,"ceiling":0.9}],"record_commit":[{"domain":"record_commit","type":"input_drive_clip","bypass":false,"drive":1.25,"clip_level":0.8},{"domain":"record_commit","type":"anti_alias_record_rate","bypass":false,"filter_family":"elliptic","sample_rate":22050,"cutoff_law":"fixed_hz","cutoff_value":9000,"order":4,"ripple_db":0.5},{"domain":"record_commit","type":"converter","bypass":false,"family":"a_law","bit_depth":10,"dac_nonlinearity":0.2,"dither_lsb":0.25,"seed":"13","seed_policy":"restart_from_profile_seed"},{"domain":"record_commit","type":"commit_stretch","bypass":false,"family":"adaptive","factor":0.75,"cycle_samples":2048,"splice_samples":128,"quality":75,"width":50,"zone_start_frame":100,"zone_end_frame":10000,"stereo_link":true}]})";

std::string replace_once(std::string source, std::string_view from,
                         std::string_view to) {
    const auto position = source.find(from);
    REQUIRE(position != std::string::npos);
    source.replace(position, from.size(), to);
    return source;
}

void require_error(std::string_view json, SampleHeritageJsonStatus status,
                   std::string_view path) {
    const auto parsed = parse_sample_heritage_profile_json(json);
    REQUIRE(parsed.status == status);
    REQUIRE(parsed.field_path == path);
}

void require_profile_error(std::string_view json,
                           SampleHeritageProfileStatus profile_status,
                           std::string_view path) {
    const auto parsed = parse_sample_heritage_profile_json(json);
    REQUIRE(parsed.status == SampleHeritageJsonStatus::ProfileValidationFailed);
    REQUIRE(parsed.profile_status == profile_status);
    REQUIRE(parsed.field_path == path);
}

}  // namespace

TEST_CASE("Sample heritage v3 JSON round-trips every typed block canonically",
          "[audio][sampler][heritage][json]") {
    const auto first = parse_sample_heritage_profile_json(canonical_json);
    REQUIRE(first.valid());
    REQUIRE(first.profile.schema_version == kSampleHeritageProfileSchemaVersion);
    REQUIRE(first.profile.voice.size() == kSampleHeritageMaximumVoiceBlocks);
    REQUIRE(first.profile.bus.size() == kSampleHeritageMaximumBusBlocks);
    REQUIRE(first.profile.record_commit.size() ==
            kSampleHeritageMaximumRecordCommitBlocks);

    REQUIRE(std::holds_alternative<SampleHeritageVoiceMachineDomainBlock>(
        first.profile.voice[0].parameters));
    REQUIRE(std::holds_alternative<SampleHeritageVoiceClockBlock>(
        first.profile.voice[1].parameters));
    REQUIRE(std::holds_alternative<SampleHeritageVoicePitchBlock>(
        first.profile.voice[2].parameters));
    REQUIRE(std::holds_alternative<SampleHeritageVoiceConverterBlock>(
        first.profile.voice[3].parameters));
    REQUIRE(std::holds_alternative<SampleHeritageVoiceLiveCyclicStretchBlock>(
        first.profile.voice[4].parameters));
    REQUIRE(std::holds_alternative<SampleHeritageVoiceHoldDroopBlock>(
        first.profile.voice[5].parameters));
    REQUIRE(std::holds_alternative<SampleHeritageVoiceReconstructionBlock>(
        first.profile.voice[6].parameters));
    REQUIRE(std::holds_alternative<SampleHeritageVoiceAnalogColorBlock>(
        first.profile.voice[7].parameters));
    REQUIRE(std::holds_alternative<SampleHeritageBusNoiseIdleBlock>(
        first.profile.bus[0].parameters));
    REQUIRE(std::holds_alternative<SampleHeritageBusOutputDriveBlock>(
        first.profile.bus[1].parameters));
    REQUIRE(std::holds_alternative<SampleHeritageRecordInputDriveClipBlock>(
        first.profile.record_commit[0].parameters));
    REQUIRE(std::holds_alternative<SampleHeritageRecordRateBlock>(
        first.profile.record_commit[1].parameters));
    REQUIRE(std::holds_alternative<SampleHeritageRecordConverterBlock>(
        first.profile.record_commit[2].parameters));
    REQUIRE(std::holds_alternative<SampleHeritageRecordCommitStretchBlock>(
        first.profile.record_commit[3].parameters));

    const auto first_validation = validate_sample_heritage_profile(first.profile);
    REQUIRE(first_validation.valid());
    const auto written = write_sample_heritage_profile_json(first.profile);
    REQUIRE(written.valid());
    REQUIRE(written.json == canonical_json);

    const auto second = parse_sample_heritage_profile_json(written.json);
    REQUIRE(second.valid());
    const auto second_validation = validate_sample_heritage_profile(second.profile);
    REQUIRE(second_validation.valid());
    REQUIRE(second_validation.profile.profile_digest ==
            first_validation.profile.profile_digest);
    REQUIRE(write_sample_heritage_profile_json(second.profile).json == written.json);
}

TEST_CASE("Sample heritage v3 digest covers every typed domain",
          "[audio][sampler][heritage][json]") {
    const auto parsed = parse_sample_heritage_profile_json(canonical_json);
    REQUIRE(parsed.valid());
    const auto original = sample_heritage_profile_digest(parsed.profile);

    auto changed = parsed.profile;
    std::get<SampleHeritageVoiceClockBlock>(changed.voice[1].parameters).ratio = 0.5;
    REQUIRE(sample_heritage_profile_digest(changed) != original);

    changed = parsed.profile;
    std::get<SampleHeritageVoiceConverterBlock>(changed.voice[3].parameters)
        .dac_nonlinearity = 0.2f;
    REQUIRE(sample_heritage_profile_digest(changed) != original);

    changed = parsed.profile;
    std::get<SampleHeritageVoiceLiveCyclicStretchBlock>(
        changed.voice[4].parameters).shuffle_divisions = 8;
    REQUIRE(sample_heritage_profile_digest(changed) != original);

    changed = parsed.profile;
    std::get<SampleHeritageVoiceReconstructionBlock>(
        changed.voice[6].parameters).cutoff_value = 0.3;
    REQUIRE(sample_heritage_profile_digest(changed) != original);

    changed = parsed.profile;
    std::get<SampleHeritageVoiceReconstructionBlock>(
        changed.voice[6].parameters).stopband_attenuation_db = 72.0f;
    REQUIRE(sample_heritage_profile_digest(changed) != original);

    changed = parsed.profile;
    std::get<SampleHeritageBusNoiseIdleBlock>(changed.bus[0].parameters)
        .tilt_db_per_octave = -6.0f;
    REQUIRE(sample_heritage_profile_digest(changed) != original);

    changed = parsed.profile;
    std::get<SampleHeritageBusNoiseIdleBlock>(changed.bus[0].parameters)
        .tilt_reference_hz = 2000.0;
    REQUIRE(sample_heritage_profile_digest(changed) != original);

    changed = parsed.profile;
    std::get<SampleHeritageBusNoiseIdleBlock>(changed.bus[0].parameters)
        .tilt_floor_hz = 40.0;
    REQUIRE(sample_heritage_profile_digest(changed) != original);

    changed = parsed.profile;
    std::get<SampleHeritageBusOutputDriveBlock>(changed.bus[1].parameters).drive = 3.0f;
    REQUIRE(sample_heritage_profile_digest(changed) != original);

    changed = parsed.profile;
    std::get<SampleHeritageRecordConverterBlock>(
        changed.record_commit[2].parameters).dac_nonlinearity = 0.3f;
    REQUIRE(sample_heritage_profile_digest(changed) != original);

    changed = parsed.profile;
    std::get<SampleHeritageRecordCommitStretchBlock>(
        changed.record_commit[3].parameters).width = 51;
    REQUIRE(sample_heritage_profile_digest(changed) != original);
}

TEST_CASE("Sample heritage v3 JSON strictly audits roots and objects",
          "[audio][sampler][heritage][json][reject]") {
    require_error("{", SampleHeritageJsonStatus::InvalidJson, "$");
    require_error("[]", SampleHeritageJsonStatus::RootNotObject, "$");
    require_error(replace_once(std::string(canonical_json),
                               "\"schema_version\":3,",
                               "\"schema_version\":3,\"schema_version\":3,"),
                  SampleHeritageJsonStatus::DuplicateField, "$");
    require_error(replace_once(std::string(canonical_json), "\"bus\":[",
                               "\"unknown\":0,\"bus\":["),
                  SampleHeritageJsonStatus::UnknownField, "$.unknown");
    require_error(replace_once(std::string(canonical_json),
                               "\"host_sample_rate\":48000,", ""),
                  SampleHeritageJsonStatus::MissingField, "$.host_sample_rate");
    require_error(replace_once(std::string(canonical_json),
                               ",\"stopband_attenuation_db\":60", ""),
                  SampleHeritageJsonStatus::MissingField,
                  "$.voice[6].stopband_attenuation_db");
    require_error(replace_once(std::string(canonical_json),
                               "\"tilt_reference_hz\":1000,", ""),
                  SampleHeritageJsonStatus::MissingField,
                  "$.bus[0].tilt_reference_hz");
    require_error(replace_once(std::string(canonical_json),
                               "\"tilt_floor_hz\":20,", ""),
                  SampleHeritageJsonStatus::MissingField,
                  "$.bus[0].tilt_floor_hz");
    require_error(
        R"({"schema_version":3,"profile_id":"neutral.empty-v3","host_sample_rate":48000,"voice":false,"bus":[],"record_commit":[]})",
        SampleHeritageJsonStatus::WrongType, "$.voice");
    require_error(replace_once(std::string(canonical_json),
                               "\"profile_id\":\"neutral.canonical-v3\"",
                               "\"profile_id\":7"),
                  SampleHeritageJsonStatus::WrongType, "$.profile_id");
    require_error(replace_once(std::string(canonical_json),
                               "\"stopband_attenuation_db\":60",
                               "\"stopband_attenuation_db\":\"60\""),
                  SampleHeritageJsonStatus::WrongType,
                  "$.voice[6].stopband_attenuation_db");
    require_error(replace_once(std::string(canonical_json),
                               "\"tilt_reference_hz\":1000",
                               "\"tilt_reference_hz\":\"1000\""),
                  SampleHeritageJsonStatus::WrongType,
                  "$.bus[0].tilt_reference_hz");
    require_error(replace_once(std::string(canonical_json),
                               "\"drive\":2,\"ceiling\":0.9",
                               "\"drive\":2,\"ceiling\":0.9,\"x\":0"),
                  SampleHeritageJsonStatus::UnknownField, "$.bus[1].x");
    require_error(replace_once(std::string(canonical_json),
                               "\"type\":\"clock\",", ""),
                  SampleHeritageJsonStatus::MissingField, "$.voice[1].type");
}

TEST_CASE("Sample heritage v3 JSON rejects wrong types enums and domains exactly",
          "[audio][sampler][heritage][json][reject]") {
    require_error(replace_once(std::string(canonical_json),
                               "\"type\":\"clock\"",
                               "\"type\":\"future_clock\""),
                  SampleHeritageJsonStatus::InvalidEnum, "$.voice[1].type");
    require_error(replace_once(std::string(canonical_json),
                               "\"family\":\"drop_repeat\"",
                               "\"family\":\"future_pitch\""),
                  SampleHeritageJsonStatus::InvalidEnum, "$.voice[2].family");
    require_error(replace_once(std::string(canonical_json),
                               "\"family\":\"mu_law\"",
                               "\"family\":\"future_converter\""),
                  SampleHeritageJsonStatus::InvalidEnum, "$.voice[3].family");
    require_error(replace_once(std::string(canonical_json),
                               "\"family\":\"elliptic\",\"cutoff_law\":\"machine_rate_ratio\"",
                               "\"family\":\"future_filter\",\"cutoff_law\":\"machine_rate_ratio\""),
                  SampleHeritageJsonStatus::InvalidEnum, "$.voice[6].family");
    require_error(replace_once(std::string(canonical_json),
                               "\"mode\":\"zero_order\"",
                               "\"mode\":\"future_hold\""),
                  SampleHeritageJsonStatus::InvalidEnum, "$.voice[5].mode");
    require_error(replace_once(std::string(canonical_json),
                               "\"cutoff_law\":\"machine_rate_ratio\"",
                               "\"cutoff_law\":\"future_law\""),
                  SampleHeritageJsonStatus::InvalidEnum,
                  "$.voice[6].cutoff_law");
    require_error(replace_once(
                      std::string(canonical_json),
                      "\"sample_rate\":22050,\"cutoff_law\":\"fixed_hz\"",
                      "\"sample_rate\":22050,\"cutoff_law\":\"future_law\""),
                  SampleHeritageJsonStatus::InvalidEnum,
                  "$.record_commit[1].cutoff_law");
    require_error(replace_once(std::string(canonical_json),
                               "\"family\":\"adaptive\"",
                               "\"family\":\"future_stretch\""),
                  SampleHeritageJsonStatus::InvalidEnum,
                  "$.record_commit[3].family");
    require_error(replace_once(std::string(canonical_json),
                               "\"domain\":\"voice\",\"type\":\"clock\"",
                               "\"domain\":\"bus\",\"type\":\"clock\""),
                  SampleHeritageJsonStatus::InvalidEnum, "$.voice[1].domain");
    require_error(replace_once(std::string(canonical_json),
                               "\"bypass\":false,\"ratio\":0.75",
                               "\"bypass\":\"false\",\"ratio\":0.75"),
                  SampleHeritageJsonStatus::WrongType, "$.voice[1].bypass");
    require_error(replace_once(std::string(canonical_json),
                               "\"seed\":\"7\"", "\"seed\":7"),
                  SampleHeritageJsonStatus::WrongType, "$.voice[3].seed");
    require_error(replace_once(std::string(canonical_json),
                               "\"seed_policy\":\"continue_serialized_state\"",
                               "\"seed_policy\":\"future_policy\""),
                  SampleHeritageJsonStatus::InvalidEnum,
                  "$.voice[3].seed_policy");
    require_error(replace_once(std::string(canonical_json),
                               "\"tempo_lock\":true",
                               "\"tempo_lock\":\"true\""),
                  SampleHeritageJsonStatus::WrongType,
                  "$.voice[4].tempo_lock");
    require_error(replace_once(std::string(canonical_json),
                               "\"shuffle_divisions\":4",
                               "\"shuffle\":true"),
                  SampleHeritageJsonStatus::UnknownField,
                  "$.voice[4].shuffle");
    require_error(replace_once(std::string(canonical_json),
                               "\"gate\":\"voice_active\"",
                               "\"gate\":\"future_gate\""),
                  SampleHeritageJsonStatus::InvalidEnum, "$.bus[0].gate");
}

TEST_CASE("Sample heritage v3 JSON reports typed block range failures exactly",
          "[audio][sampler][heritage][json][reject]") {
    require_error(replace_once(std::string(canonical_json),
                               "\"sample_rate\":32000", "\"sample_rate\":7999"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.voice[0].sample_rate");
    require_error(replace_once(std::string(canonical_json), "\"ratio\":0.75",
                               "\"ratio\":0"),
                  SampleHeritageJsonStatus::NumberOutOfRange, "$.voice[1].ratio");
    require_error(replace_once(std::string(canonical_json), "\"bit_depth\":12",
                               "\"bit_depth\":3"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.voice[3].bit_depth");
    require_error(replace_once(std::string(canonical_json),
                               "\"dac_nonlinearity\":0.1",
                               "\"dac_nonlinearity\":1.01"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.voice[3].dac_nonlinearity");
    require_error(replace_once(std::string(canonical_json),
                               "\"seed\":\"7\"", "\"seed\":\"0\""),
                  SampleHeritageJsonStatus::NumberOutOfRange, "$.voice[3].seed");
    require_error(replace_once(std::string(canonical_json),
                               "\"cycle_ms\":40", "\"cycle_ms\":0"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.voice[4].cycle_ms");
    require_profile_error(replace_once(std::string(canonical_json),
                                       "\"cycle_ms\":40", "\"cycle_ms\":40000"),
                          SampleHeritageProfileStatus::InvalidStageParameter,
                          "$.voice[4]");
    require_error(replace_once(std::string(canonical_json),
                               "\"cycle_ms\":40,\"splice_ms\":2",
                               "\"cycle_ms\":4,\"splice_ms\":2.01"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.voice[4].splice_ms");
    require_error(replace_once(std::string(canonical_json),
                               "\"shuffle_divisions\":4",
                               "\"shuffle_divisions\":1"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.voice[4].shuffle_divisions");
    require_error(replace_once(std::string(canonical_json),
                               "\"shuffle_divisions\":4,\"seed\":\"9\"",
                               "\"shuffle_divisions\":4,\"seed\":\"0\""),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.voice[4].seed");
    require_error(replace_once(std::string(canonical_json),
                               "\"hold_samples\":2", "\"hold_samples\":0"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.voice[5].hold_samples");
    require_error(replace_once(std::string(canonical_json),
                               "\"cutoff_value\":0.4", "\"cutoff_value\":0.5"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.voice[6].cutoff_value");
    require_error(replace_once(std::string(canonical_json),
                               "\"stopband_attenuation_db\":60",
                               "\"stopband_attenuation_db\":0.5"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.voice[6].stopband_attenuation_db");
    require_error(replace_once(std::string(canonical_json),
                               "\"stopband_attenuation_db\":60",
                               "\"stopband_attenuation_db\":180.01"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.voice[6].stopband_attenuation_db");
    require_error(replace_once(std::string(canonical_json), "\"order\":4",
                               "\"order\":3"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.voice[6].order");
    require_error(replace_once(std::string(canonical_json), "\"mix\":0.75",
                               "\"mix\":1.01"),
                  SampleHeritageJsonStatus::NumberOutOfRange, "$.voice[7].mix");
    require_error(replace_once(std::string(canonical_json),
                               "\"noise_amplitude\":0.01",
                               "\"noise_amplitude\":1.01"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.bus[0].noise_amplitude");
    require_error(replace_once(std::string(canonical_json),
                               "\"tilt_db_per_octave\":-3",
                               "\"tilt_db_per_octave\":-24.01"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.bus[0].tilt_db_per_octave");
    require_error(replace_once(std::string(canonical_json),
                               "\"tilt_reference_hz\":1000",
                               "\"tilt_reference_hz\":24000"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.bus[0].tilt_reference_hz");
    require_error(replace_once(std::string(canonical_json),
                               "\"tilt_floor_hz\":20",
                               "\"tilt_floor_hz\":1001"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.bus[0].tilt_floor_hz");
    require_error(replace_once(std::string(canonical_json), "\"ceiling\":0.9",
                               "\"ceiling\":0"),
                  SampleHeritageJsonStatus::NumberOutOfRange, "$.bus[1].ceiling");
    require_error(replace_once(std::string(canonical_json),
                               "\"clip_level\":0.8", "\"clip_level\":0"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.record_commit[0].clip_level");
    require_error(replace_once(std::string(canonical_json),
                               "\"cutoff_value\":9000",
                               "\"cutoff_value\":11025"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.record_commit[1].cutoff_value");
    require_error(replace_once(std::string(canonical_json), "\"bit_depth\":10",
                               "\"bit_depth\":17"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.record_commit[2].bit_depth");
    require_error(replace_once(std::string(canonical_json),
                               "\"dac_nonlinearity\":0.2",
                               "\"dac_nonlinearity\":-0.01"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.record_commit[2].dac_nonlinearity");
    require_error(replace_once(std::string(canonical_json),
                               "\"factor\":0.75,\"cycle_samples\":2048",
                               "\"factor\":20.01,\"cycle_samples\":2048"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.record_commit[3].factor");
}

TEST_CASE("Sample heritage v3 commit stretch cross-fields are explicit",
          "[audio][sampler][heritage][json][reject]") {
    require_error(replace_once(std::string(canonical_json), "\"quality\":75",
                               "\"quality\":0"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.record_commit[3].quality");
    require_error(replace_once(std::string(canonical_json), "\"width\":50",
                               "\"width\":0"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.record_commit[3].width");
    require_error(replace_once(std::string(canonical_json),
                               "\"zone_start_frame\":100,\"zone_end_frame\":10000",
                               "\"zone_start_frame\":10000,\"zone_end_frame\":100"),
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.record_commit[3].zone_end_frame");

    auto cyclic = replace_once(std::string(canonical_json),
                               "\"family\":\"adaptive\"",
                               "\"family\":\"cyclic\"");
    cyclic = replace_once(std::move(cyclic), "\"quality\":75", "\"quality\":1");
    require_error(cyclic, SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.record_commit[3].quality");

    cyclic = replace_once(std::string(canonical_json),
                          "\"family\":\"adaptive\"",
                          "\"family\":\"cyclic\"");
    cyclic = replace_once(std::move(cyclic), "\"quality\":75", "\"quality\":0");
    cyclic = replace_once(std::move(cyclic), "\"width\":50", "\"width\":0");
    REQUIRE(parse_sample_heritage_profile_json(cyclic).valid());
}

TEST_CASE("Sample heritage v3 reconstruction families require complete design parameters",
          "[audio][sampler][heritage][json][reject]") {
    auto butterworth = replace_once(std::string(canonical_json),
                                    "\"family\":\"elliptic\",\"cutoff_law\"",
                                    "\"family\":\"butterworth\",\"cutoff_law\"");
    butterworth = replace_once(std::move(butterworth), "\"ripple_db\":0.5",
                               "\"ripple_db\":0");
    butterworth = replace_once(std::move(butterworth),
                               "\"stopband_attenuation_db\":60",
                               "\"stopband_attenuation_db\":0");
    REQUIRE(parse_sample_heritage_profile_json(butterworth).valid());

    auto chebyshev = replace_once(std::string(canonical_json),
                                  "\"family\":\"elliptic\",\"cutoff_law\"",
                                  "\"family\":\"chebyshev\",\"cutoff_law\"");
    chebyshev = replace_once(std::move(chebyshev),
                             "\"stopband_attenuation_db\":60",
                             "\"stopband_attenuation_db\":0");
    REQUIRE(parse_sample_heritage_profile_json(chebyshev).valid());

    auto one_pole = replace_once(std::string(canonical_json),
                                 "\"family\":\"elliptic\",\"cutoff_law\"",
                                 "\"family\":\"one_pole\",\"cutoff_law\"");
    one_pole = replace_once(std::move(one_pole), "\"order\":4", "\"order\":1");
    one_pole = replace_once(std::move(one_pole), "\"ripple_db\":0.5",
                            "\"ripple_db\":0");
    one_pole = replace_once(std::move(one_pole),
                            "\"stopband_attenuation_db\":60",
                            "\"stopband_attenuation_db\":0");
    REQUIRE(parse_sample_heritage_profile_json(one_pole).valid());

    auto invalid_butterworth = replace_once(
        std::string(canonical_json),
        "\"family\":\"elliptic\",\"cutoff_law\"",
        "\"family\":\"butterworth\",\"cutoff_law\"");
    require_error(invalid_butterworth,
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.voice[6].ripple_db");

    auto invalid_chebyshev = replace_once(
        std::string(canonical_json),
        "\"family\":\"elliptic\",\"cutoff_law\"",
        "\"family\":\"chebyshev\",\"cutoff_law\"");
    require_error(invalid_chebyshev,
                  SampleHeritageJsonStatus::NumberOutOfRange,
                  "$.voice[6].stopband_attenuation_db");

    const auto parsed = parse_sample_heritage_profile_json(canonical_json);
    REQUIRE(parsed.valid());
    auto invalid_profile = parsed.profile;
    auto& reconstruction = std::get<SampleHeritageVoiceReconstructionBlock>(
        invalid_profile.voice[6].parameters);
    reconstruction.stopband_attenuation_db = reconstruction.ripple_db;
    const auto validation = validate_sample_heritage_profile(invalid_profile);
    REQUIRE_FALSE(validation.valid());
    REQUIRE(validation.status ==
            SampleHeritageProfileStatus::InvalidStageParameter);
    REQUIRE(validation.block_domain == SampleHeritageBlockDomain::Voice);
    REQUIRE(validation.stage_index == 6);
}

TEST_CASE("Sample heritage v3 JSON rejects duplicate and unordered typed blocks",
          "[audio][sampler][heritage][json][reject]") {
    constexpr std::string_view clock =
        R"({"domain":"voice","type":"clock","bypass":false,"ratio":0.75})";
    constexpr std::string_view pitch =
        R"({"domain":"voice","type":"pitch","bypass":false,"family":"drop_repeat"})";

    require_profile_error(
        replace_once(std::string(canonical_json), pitch, clock),
        SampleHeritageProfileStatus::DuplicateStage, "$.voice[2]");
    require_profile_error(
        replace_once(std::string(canonical_json),
                     std::string(clock) + "," + std::string(pitch),
                     std::string(pitch) + "," + std::string(clock)),
        SampleHeritageProfileStatus::InvalidStageOrder, "$.voice[2]");
}

TEST_CASE("Sample heritage v3 writer rejects noncanonical in-memory forms",
          "[audio][sampler][heritage][json][reject]") {
    const auto parsed = parse_sample_heritage_profile_json(canonical_json);
    REQUIRE(parsed.valid());

    auto invalid = parsed.profile;
    invalid.profile_id = "brand.machine";
    auto written = write_sample_heritage_profile_json(invalid);
    REQUIRE_FALSE(written.valid());
    REQUIRE(written.json.empty());
    REQUIRE(written.profile_status == SampleHeritageProfileStatus::InvalidProfileId);

    invalid = parsed.profile;
    invalid.stages.push_back({false, SampleHeritageOutputStage{1.0f}});
    written = write_sample_heritage_profile_json(invalid);
    REQUIRE_FALSE(written.valid());
    REQUIRE(written.json.empty());
    REQUIRE(written.profile_status ==
            SampleHeritageProfileStatus::MixedLegacyAndTypedBlocks);

    SampleHeritageProfile flat{
        .schema_version = kSampleHeritageProfileSchemaVersion,
        .profile_id = "neutral.flat-memory-only",
        .host_sample_rate = 48000.0,
        .stages = {{false, SampleHeritageOutputStage{1.0f}}},
    };
    written = write_sample_heritage_profile_json(flat);
    REQUIRE_FALSE(written.valid());
    REQUIRE(written.json.empty());
    REQUIRE(written.profile_status ==
            SampleHeritageProfileStatus::NonCanonicalProfileRepresentation);
}

TEST_CASE("Sample heritage v3 boundary neighbors remain accepted",
          "[audio][sampler][heritage][json]") {
    auto valid = replace_once(std::string(canonical_json),
                              "\"bit_depth\":12", "\"bit_depth\":4");
    REQUIRE(parse_sample_heritage_profile_json(valid).valid());
    valid = replace_once(std::string(canonical_json),
                         "\"dac_nonlinearity\":0.1",
                         "\"dac_nonlinearity\":1");
    REQUIRE(parse_sample_heritage_profile_json(valid).valid());
    valid = replace_once(std::string(canonical_json),
                         "\"cycle_ms\":40,\"splice_ms\":2",
                         "\"cycle_ms\":40,\"splice_ms\":20");
    REQUIRE(parse_sample_heritage_profile_json(valid).valid());
    valid = replace_once(std::string(canonical_json),
                         "\"tempo_lock\":true,\"shuffle_divisions\":4",
                         "\"tempo_lock\":false,\"shuffle_divisions\":0");
    REQUIRE(parse_sample_heritage_profile_json(valid).valid());
    valid = replace_once(std::string(canonical_json),
                         "\"noise_amplitude\":0.01",
                         "\"noise_amplitude\":1");
    REQUIRE(parse_sample_heritage_profile_json(valid).valid());
    valid = replace_once(
        std::string(canonical_json),
        "\"tilt_db_per_octave\":-3,\"tilt_reference_hz\":1000,\"tilt_floor_hz\":20,\"gate\":\"voice_active\"",
        "\"tilt_db_per_octave\":24,\"tilt_reference_hz\":23999.999,\"tilt_floor_hz\":1,\"gate\":\"always_on\"");
    REQUIRE(parse_sample_heritage_profile_json(valid).valid());
    valid = replace_once(std::string(canonical_json),
                         "\"stopband_attenuation_db\":60",
                         "\"stopband_attenuation_db\":180");
    REQUIRE(parse_sample_heritage_profile_json(valid).valid());
    valid = replace_once(std::string(canonical_json), "\"ceiling\":0.9",
                         "\"ceiling\":0.001");
    REQUIRE(parse_sample_heritage_profile_json(valid).valid());
    valid = replace_once(std::string(canonical_json),
                         "\"cutoff_value\":0.4",
                         "\"cutoff_value\":0.499999");
    REQUIRE(parse_sample_heritage_profile_json(valid).valid());
    valid = replace_once(std::string(canonical_json),
                         "\"cutoff_value\":9000",
                         "\"cutoff_value\":11024.999");
    REQUIRE(parse_sample_heritage_profile_json(valid).valid());
    valid = replace_once(std::string(canonical_json),
                         "\"factor\":0.75,\"cycle_samples\":2048",
                         "\"factor\":20,\"cycle_samples\":2048");
    REQUIRE(parse_sample_heritage_profile_json(valid).valid());
}
