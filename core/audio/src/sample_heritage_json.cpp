#include <pulp/audio/sample_heritage_json.hpp>

#include <choc/text/choc_JSON.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace pulp::audio {
namespace {

using namespace std::string_view_literals;
using Value = choc::value::ValueView;

template<typename Result>
struct JsonParserBase {
    Result result;

    bool fail(SampleHeritageJsonStatus status, std::string path) {
        result.status = status;
        result.field_path = std::move(path);
        return false;
    }

    template<std::size_t N>
    bool audit_object(Value object,
                      std::string_view path,
                      const std::array<std::string_view, N>& fields) {
        if (!object.isObject()) return fail(SampleHeritageJsonStatus::WrongType,
                                            std::string(path));
        for (std::uint32_t index = 0; index < object.size(); ++index) {
            const auto member = object.getObjectMemberAt(index);
            bool known = false;
            for (const auto field : fields) known = known || member.name == field;
            const auto member_path = std::string(path) + "." + std::string(member.name);
            if (!known) return fail(SampleHeritageJsonStatus::UnknownField, member_path);
            for (std::uint32_t earlier = 0; earlier < index; ++earlier) {
                if (object.getObjectMemberAt(earlier).name == member.name)
                    return fail(SampleHeritageJsonStatus::DuplicateField, member_path);
            }
        }
        for (const auto field : fields) {
            if (!object.hasObjectMember(field))
                return fail(SampleHeritageJsonStatus::MissingField,
                            std::string(path) + "." + std::string(field));
        }
        return true;
    }

    bool string(Value object, std::string_view field, std::string_view path,
                std::string& destination) {
        const auto value = object[field];
        if (!value.isString()) return fail(SampleHeritageJsonStatus::WrongType,
                                           std::string(path));
        destination = value.getString();
        return true;
    }

    template<typename Integer>
    bool integer(Value object, std::string_view field, std::string_view path,
                 std::uint64_t minimum, std::uint64_t maximum,
                 Integer& destination) {
        const auto value = object[field];
        if (!value.isInt()) return fail(SampleHeritageJsonStatus::WrongType,
                                        std::string(path));
        const auto signed_value = value.get<int64_t>();
        if (signed_value < 0 || static_cast<std::uint64_t>(signed_value) < minimum ||
            static_cast<std::uint64_t>(signed_value) > maximum)
            return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                        std::string(path));
        destination = static_cast<Integer>(signed_value);
        return true;
    }
};

struct Parser : JsonParserBase<SampleHeritageJsonParseResult> {

    bool boolean(Value object, std::string_view field, std::string_view path,
                 bool& destination) {
        const auto value = object[field];
        if (!value.isBool()) return fail(SampleHeritageJsonStatus::WrongType,
                                         std::string(path));
        destination = value.getBool();
        return true;
    }

    bool number(Value object, std::string_view field, std::string_view path,
                double minimum, double maximum, double& destination) {
        const auto value = object[field];
        if (!value.isInt() && !value.isFloat())
            return fail(SampleHeritageJsonStatus::WrongType, std::string(path));
        destination = value.isInt() ? static_cast<double>(value.get<int64_t>())
                                    : value.get<double>();
        if (!std::isfinite(destination) || destination < minimum || destination > maximum)
            return fail(SampleHeritageJsonStatus::NumberOutOfRange, std::string(path));
        return true;
    }

    bool seed(Value object, std::string_view field, std::string_view path,
              std::uint64_t& destination) {
        const auto value = object[field];
        if (!value.isString()) return fail(SampleHeritageJsonStatus::WrongType,
                                           std::string(path));
        const auto text = value.getString();
        if (text.empty() || (text.size() > 1 && text.front() == '0'))
            return fail(SampleHeritageJsonStatus::NumberOutOfRange, std::string(path));
        const auto conversion = std::from_chars(text.data(), text.data() + text.size(),
                                                destination);
        if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size())
            return fail(SampleHeritageJsonStatus::NumberOutOfRange, std::string(path));
        return true;
    }

    bool seed_policy(Value object, std::string_view field, std::string_view path,
                     SampleHeritageSeedPolicy& destination) {
        const auto value = object[field];
        if (!value.isString()) return fail(SampleHeritageJsonStatus::WrongType,
                                           std::string(path));
        const auto text = value.getString();
        if (text == "restart_from_profile_seed") {
            destination = SampleHeritageSeedPolicy::RestartFromProfileSeed;
            return true;
        }
        if (text == "continue_serialized_state") {
            destination = SampleHeritageSeedPolicy::ContinueSerializedState;
            return true;
        }
        return fail(SampleHeritageJsonStatus::InvalidEnum, std::string(path));
    }

    bool stage(Value value, std::size_t index, SampleHeritageStageSpec& destination) {
        const auto base = "$.stages[" + std::to_string(index) + "]";
        if (!value.isObject()) return fail(SampleHeritageJsonStatus::WrongType, base);
        if (!value.hasObjectMember("type"))
            return fail(SampleHeritageJsonStatus::MissingField, base + ".type");
        if (!value["type"].isString())
            return fail(SampleHeritageJsonStatus::WrongType, base + ".type");
        const auto type = value["type"].getString();

        if (type == "machine_domain") {
            constexpr std::array fields{"type"sv, "bypass"sv, "sample_rate"sv};
            double sample_rate = 0.0;
            if (!audit_object(value, base, fields) ||
                !boolean(value, "bypass", base + ".bypass", destination.bypass) ||
                !number(value, "sample_rate", base + ".sample_rate",
                        std::numeric_limits<double>::denorm_min(),
                        std::numeric_limits<double>::max(), sample_rate)) return false;
            destination.parameters = SampleHeritageMachineDomainStage{sample_rate};
        } else if (type == "quantization") {
            constexpr std::array fields{"type"sv, "bypass"sv, "bit_depth"sv,
                                        "dither_lsb"sv, "seed"sv, "seed_policy"sv};
            std::uint8_t bit_depth = 0;
            double dither = 0.0;
            std::uint64_t seed_value = 0;
            SampleHeritageSeedPolicy policy{};
            if (!audit_object(value, base, fields) ||
                !boolean(value, "bypass", base + ".bypass", destination.bypass) ||
                !integer(value, "bit_depth", base + ".bit_depth", 2, 24, bit_depth) ||
                !number(value, "dither_lsb", base + ".dither_lsb", 0.0, 2.0, dither) ||
                !seed(value, "seed", base + ".seed", seed_value) ||
                !seed_policy(value, "seed_policy", base + ".seed_policy", policy)) return false;
            destination.parameters = SampleHeritageQuantizationStage{
                bit_depth, static_cast<float>(dither), seed_value, policy};
        } else if (type == "clock_pitch") {
            constexpr std::array fields{"type"sv, "bypass"sv, "ratio"sv};
            double ratio = 0.0;
            if (!audit_object(value, base, fields) ||
                !boolean(value, "bypass", base + ".bypass", destination.bypass) ||
                !number(value, "ratio", base + ".ratio",
                        std::numeric_limits<double>::denorm_min(),
                        std::numeric_limits<double>::max(), ratio)) return false;
            destination.parameters = SampleHeritageClockPitchStage{ratio};
        } else if (type == "dac_hold") {
            constexpr std::array fields{"type"sv, "bypass"sv, "hold_samples"sv};
            std::uint32_t hold_samples = 0;
            if (!audit_object(value, base, fields) ||
                !boolean(value, "bypass", base + ".bypass", destination.bypass) ||
                !integer(value, "hold_samples", base + ".hold_samples", 1, 65536,
                         hold_samples)) return false;
            destination.parameters = SampleHeritageDacHoldStage{hold_samples};
        } else if (type == "reconstruction_filter") {
            constexpr std::array fields{"type"sv, "bypass"sv, "cutoff_hz"sv};
            double cutoff = 0.0;
            if (!audit_object(value, base, fields) ||
                !boolean(value, "bypass", base + ".bypass", destination.bypass) ||
                !number(value, "cutoff_hz", base + ".cutoff_hz",
                        std::numeric_limits<double>::denorm_min(),
                        std::numeric_limits<double>::max(), cutoff)) return false;
            destination.parameters = SampleHeritageReconstructionFilterStage{cutoff};
        } else if (type == "noise") {
            constexpr std::array fields{"type"sv, "bypass"sv, "amplitude"sv,
                                        "seed"sv, "seed_policy"sv};
            double amplitude = 0.0;
            std::uint64_t seed_value = 0;
            SampleHeritageSeedPolicy policy{};
            if (!audit_object(value, base, fields) ||
                !boolean(value, "bypass", base + ".bypass", destination.bypass) ||
                !number(value, "amplitude", base + ".amplitude", 0.0, 1.0, amplitude) ||
                !seed(value, "seed", base + ".seed", seed_value) ||
                !seed_policy(value, "seed_policy", base + ".seed_policy", policy)) return false;
            destination.parameters = SampleHeritageNoiseStage{
                static_cast<float>(amplitude), seed_value, policy};
        } else if (type == "output") {
            constexpr std::array fields{"type"sv, "bypass"sv, "gain"sv};
            double gain = 0.0;
            if (!audit_object(value, base, fields) ||
                !boolean(value, "bypass", base + ".bypass", destination.bypass) ||
                !number(value, "gain", base + ".gain", 0.0, 16.0, gain)) return false;
            destination.parameters = SampleHeritageOutputStage{static_cast<float>(gain)};
        } else {
            return fail(SampleHeritageJsonStatus::InvalidEnum, base + ".type");
        }
        return true;
    }
};

struct RuntimeStateParser
    : JsonParserBase<SampleHeritageRuntimeStateJsonParseResult> {

    bool random_state(Value object,
                      std::string_view path,
                      std::uint64_t& destination) {
        const auto value = object["random_state"];
        if (!value.isString())
            return fail(SampleHeritageJsonStatus::WrongType, std::string(path));
        const auto text = value.getString();
        if (text.empty() || text == "0" || (text.size() > 1 && text.front() == '0'))
            return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                        std::string(path));
        const auto conversion = std::from_chars(
            text.data(), text.data() + text.size(), destination);
        if (conversion.ec != std::errc{} ||
            conversion.ptr != text.data() + text.size())
            return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                        std::string(path));
        return true;
    }
};

SampleHeritageRuntimeStateStatus validate_runtime_state_shape(
    const SampleHeritageRuntimeState& state) noexcept {
    if (state.schema_version != kSampleHeritageRuntimeStateSchemaVersion)
        return SampleHeritageRuntimeStateStatus::UnsupportedSchemaVersion;
    if (state.profile_schema_version != kSampleHeritageProfileSchemaVersion ||
        state.profile_digest_version != kSampleHeritageProfileDigestVersion ||
        !detail::valid_neutral_profile_id(state.bound_profile_id()))
        return SampleHeritageRuntimeStateStatus::ProfileMismatch;
    if (state.rng_state_count > state.rng_states.size())
        return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
    for (std::size_t index = 0; index < state.rng_state_count; ++index) {
        const auto& rng = state.rng_states[index];
        if (rng.stage_index >= kSampleHeritageMaximumStages ||
            (index != 0 &&
             state.rng_states[index - 1].stage_index >= rng.stage_index) ||
            (rng.stage_type != SampleHeritageRuntimeRngStageType::Quantization &&
             rng.stage_type != SampleHeritageRuntimeRngStageType::Noise))
            return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
        if (rng.random_state == 0)
            return SampleHeritageRuntimeStateStatus::InvalidRandomState;
    }
    return SampleHeritageRuntimeStateStatus::Ok;
}

bool parse_digest_hex(std::string_view text,
                      std::array<std::uint8_t, 32>& digest) noexcept {
    if (text.size() != digest.size() * 2) return false;
    const auto nibble = [](char character) noexcept -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return 10 + character - 'a';
        return -1;
    };
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto high = nibble(text[index * 2]);
        const auto low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        digest[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

void append_digest_hex(std::string& output,
                       const std::array<std::uint8_t, 32>& digest) {
    constexpr std::string_view digits = "0123456789abcdef";
    output.push_back('"');
    for (const auto byte : digest) {
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 0x0f]);
    }
    output.push_back('"');
}

template<typename Number>
void append_number(std::string& output, Number value) {
    if (value == Number{}) value = Number{};
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                         std::chars_format::general,
                                         std::numeric_limits<Number>::max_digits10);
    output.append(buffer.data(), converted.ptr);
}

void append_seed(std::string& output, std::uint64_t seed) {
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), seed);
    output.push_back('"');
    output.append(buffer.data(), converted.ptr);
    output.push_back('"');
}

std::string_view seed_policy_name(SampleHeritageSeedPolicy policy) {
    return policy == SampleHeritageSeedPolicy::RestartFromProfileSeed
        ? "restart_from_profile_seed"
        : "continue_serialized_state";
}

}  // namespace

SampleHeritageJsonParseResult parse_sample_heritage_profile_json(std::string_view json) {
    Parser parser;
    try {
        const auto root_value = choc::json::parse(json);
        const Value root = root_value;
        if (!root.isObject()) {
            parser.fail(SampleHeritageJsonStatus::RootNotObject, "$");
            return std::move(parser.result);
        }
        constexpr std::array fields{"schema_version"sv, "profile_id"sv,
                                    "host_sample_rate"sv, "stages"sv};
        if (!parser.audit_object(root, "$", fields)) return std::move(parser.result);

        std::uint32_t schema = 0;
        if (!parser.integer(root, "schema_version", "$.schema_version", 0,
                            std::numeric_limits<std::uint32_t>::max(), schema))
            return std::move(parser.result);
        if (schema != kSampleHeritageProfileSchemaVersion) {
            parser.result.profile_status =
                SampleHeritageProfileStatus::UnsupportedSchemaVersion;
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.schema_version");
            return std::move(parser.result);
        }
        parser.result.profile.schema_version = schema;
        if (!parser.string(root, "profile_id", "$.profile_id",
                           parser.result.profile.profile_id))
            return std::move(parser.result);
        if (!parser.number(root, "host_sample_rate", "$.host_sample_rate", 8000.0,
                           384000.0, parser.result.profile.host_sample_rate))
            return std::move(parser.result);
        const auto stages = root["stages"];
        if (!stages.isArray()) {
            parser.fail(SampleHeritageJsonStatus::WrongType, "$.stages");
            return std::move(parser.result);
        }
        if (stages.size() > kSampleHeritageMaximumStages) {
            parser.fail(SampleHeritageJsonStatus::NumberOutOfRange, "$.stages");
            return std::move(parser.result);
        }
        parser.result.profile.stages.resize(stages.size());
        for (std::uint32_t index = 0; index < stages.size(); ++index) {
            if (!parser.stage(stages[index], index, parser.result.profile.stages[index]))
                return std::move(parser.result);
        }
        const auto validation = validate_sample_heritage_profile(parser.result.profile);
        parser.result.profile_status = validation.status;
        if (!validation.valid()) {
            std::string path = "$";
            if (validation.status == SampleHeritageProfileStatus::InvalidProfileId)
                path = "$.profile_id";
            else if (validation.status ==
                     SampleHeritageProfileStatus::InvalidHostSampleRate)
                path = "$.host_sample_rate";
            else if (validation.status ==
                     SampleHeritageProfileStatus::UnsupportedSchemaVersion)
                path = "$.schema_version";
            else if (validation.stage_index < parser.result.profile.stages.size())
                path = "$.stages[" + std::to_string(validation.stage_index) + "]";
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        std::move(path));
            return std::move(parser.result);
        }
        parser.result.status = SampleHeritageJsonStatus::Ok;
        parser.result.field_path.clear();
        return std::move(parser.result);
    } catch (const choc::value::Error& error) {
        parser.result.status =
            std::string_view(error.what()) ==
                    "This object already contains a member with the given name"
                ? SampleHeritageJsonStatus::DuplicateField
                : SampleHeritageJsonStatus::InvalidJson;
        // CHOC rejects a duplicate while constructing the value, before it can
        // expose the containing object. The status remains exact; "$" is the
        // narrowest trustworthy path available at that point.
        parser.result.field_path = "$";
        return std::move(parser.result);
    } catch (const choc::json::ParseError&) {
        parser.result.status = SampleHeritageJsonStatus::InvalidJson;
        parser.result.field_path = "$";
        return std::move(parser.result);
    }
}

SampleHeritageJsonWriteResult write_sample_heritage_profile_json(
    const SampleHeritageProfile& profile) {
    SampleHeritageJsonWriteResult result;
    const auto validation = validate_sample_heritage_profile(profile);
    result.profile_status = validation.status;
    if (!validation.valid()) return result;

    auto& output = result.json;
    output = "{\"schema_version\":1,\"profile_id\":\"" + profile.profile_id +
             "\",\"host_sample_rate\":";
    append_number(output, profile.host_sample_rate);
    output += ",\"stages\":[";
    for (std::size_t index = 0; index < profile.stages.size(); ++index) {
        if (index != 0) output.push_back(',');
        const auto& spec = profile.stages[index];
        std::visit([&](const auto& stage) {
            using Stage = std::decay_t<decltype(stage)>;
            output += "{\"type\":\"";
            if constexpr (std::is_same_v<Stage, SampleHeritageMachineDomainStage>)
                output += "machine_domain\",\"bypass\":";
            else if constexpr (std::is_same_v<Stage, SampleHeritageQuantizationStage>)
                output += "quantization\",\"bypass\":";
            else if constexpr (std::is_same_v<Stage, SampleHeritageClockPitchStage>)
                output += "clock_pitch\",\"bypass\":";
            else if constexpr (std::is_same_v<Stage, SampleHeritageDacHoldStage>)
                output += "dac_hold\",\"bypass\":";
            else if constexpr (std::is_same_v<Stage, SampleHeritageReconstructionFilterStage>)
                output += "reconstruction_filter\",\"bypass\":";
            else if constexpr (std::is_same_v<Stage, SampleHeritageNoiseStage>)
                output += "noise\",\"bypass\":";
            else
                output += "output\",\"bypass\":";
            output += spec.bypass ? "true" : "false";
            if constexpr (std::is_same_v<Stage, SampleHeritageMachineDomainStage>) {
                output += ",\"sample_rate\":"; append_number(output, stage.sample_rate);
            } else if constexpr (std::is_same_v<Stage, SampleHeritageQuantizationStage>) {
                output += ",\"bit_depth\":" + std::to_string(stage.bit_depth);
                output += ",\"dither_lsb\":"; append_number(output, stage.dither_lsb);
                output += ",\"seed\":"; append_seed(output, stage.seed);
                output += ",\"seed_policy\":\"";
                output += seed_policy_name(stage.seed_policy); output.push_back('"');
            } else if constexpr (std::is_same_v<Stage, SampleHeritageClockPitchStage>) {
                output += ",\"ratio\":"; append_number(output, stage.ratio);
            } else if constexpr (std::is_same_v<Stage, SampleHeritageDacHoldStage>) {
                output += ",\"hold_samples\":" + std::to_string(stage.hold_samples);
            } else if constexpr (std::is_same_v<Stage, SampleHeritageReconstructionFilterStage>) {
                output += ",\"cutoff_hz\":"; append_number(output, stage.cutoff_hz);
            } else if constexpr (std::is_same_v<Stage, SampleHeritageNoiseStage>) {
                output += ",\"amplitude\":"; append_number(output, stage.amplitude);
                output += ",\"seed\":"; append_seed(output, stage.seed);
                output += ",\"seed_policy\":\"";
                output += seed_policy_name(stage.seed_policy); output.push_back('"');
            } else if constexpr (std::is_same_v<Stage, SampleHeritageOutputStage>) {
                output += ",\"gain\":"; append_number(output, stage.gain);
            }
            output.push_back('}');
        }, spec.parameters);
    }
    output += "]}";
    result.status = SampleHeritageJsonStatus::Ok;
    return result;
}

SampleHeritageRuntimeStateJsonParseResult
parse_sample_heritage_runtime_state_json(std::string_view json) {
    RuntimeStateParser parser;
    try {
        const auto root_value = choc::json::parse(json);
        const Value root = root_value;
        if (!root.isObject()) {
            parser.fail(SampleHeritageJsonStatus::RootNotObject, "$");
            return std::move(parser.result);
        }
        constexpr std::array fields{
            "schema_version"sv, "profile_schema_version"sv, "profile_id"sv,
            "profile_digest_version"sv, "profile_digest"sv, "rng_states"sv};
        if (!parser.audit_object(root, "$", fields))
            return std::move(parser.result);

        std::uint32_t schema_version = 0;
        if (!parser.integer(root, "schema_version", "$.schema_version", 0,
                            std::numeric_limits<std::uint32_t>::max(),
                            schema_version))
            return std::move(parser.result);
        parser.result.state.schema_version = schema_version;
        if (schema_version != kSampleHeritageRuntimeStateSchemaVersion) {
            parser.result.runtime_status =
                SampleHeritageRuntimeStateStatus::UnsupportedSchemaVersion;
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.schema_version");
            return std::move(parser.result);
        }
        if (!parser.integer(root, "profile_schema_version",
                            "$.profile_schema_version", 0,
                            std::numeric_limits<std::uint32_t>::max(),
                            parser.result.state.profile_schema_version))
            return std::move(parser.result);
        if (parser.result.state.profile_schema_version !=
            kSampleHeritageProfileSchemaVersion) {
            parser.result.runtime_status =
                SampleHeritageRuntimeStateStatus::ProfileMismatch;
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.profile_schema_version");
            return std::move(parser.result);
        }
        std::string profile_id;
        if (!parser.string(root, "profile_id", "$.profile_id", profile_id))
            return std::move(parser.result);
        if (!detail::valid_neutral_profile_id(profile_id)) {
            parser.result.runtime_status =
                SampleHeritageRuntimeStateStatus::ProfileMismatch;
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.profile_id");
            return std::move(parser.result);
        }
        std::copy(profile_id.begin(), profile_id.end(),
                  parser.result.state.profile_id.begin());
        if (!parser.integer(root, "profile_digest_version",
                            "$.profile_digest_version", 0,
                            std::numeric_limits<std::uint32_t>::max(),
                            parser.result.state.profile_digest_version))
            return std::move(parser.result);
        if (parser.result.state.profile_digest_version !=
            kSampleHeritageProfileDigestVersion) {
            parser.result.runtime_status =
                SampleHeritageRuntimeStateStatus::ProfileMismatch;
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.profile_digest_version");
            return std::move(parser.result);
        }
        std::string profile_digest;
        if (!parser.string(root, "profile_digest", "$.profile_digest",
                           profile_digest))
            return std::move(parser.result);
        if (!parse_digest_hex(profile_digest,
                              parser.result.state.profile_digest)) {
            parser.result.runtime_status =
                SampleHeritageRuntimeStateStatus::ProfileMismatch;
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.profile_digest");
            return std::move(parser.result);
        }

        const auto states = root["rng_states"];
        if (!states.isArray()) {
            parser.fail(SampleHeritageJsonStatus::WrongType, "$.rng_states");
            return std::move(parser.result);
        }
        if (states.size() > kSampleHeritageMaximumStages) {
            parser.fail(SampleHeritageJsonStatus::NumberOutOfRange,
                        "$.rng_states");
            return std::move(parser.result);
        }
        parser.result.state.rng_state_count = states.size();
        for (std::uint32_t index = 0; index < states.size(); ++index) {
            const auto value = states[index];
            const auto base = "$.rng_states[" + std::to_string(index) + "]";
            constexpr std::array state_fields{
                "stage_index"sv, "stage_type"sv, "random_state"sv};
            if (!parser.audit_object(value, base, state_fields))
                return std::move(parser.result);
            std::uint32_t stage_index = 0;
            if (!parser.integer(value, "stage_index", base + ".stage_index", 0,
                                kSampleHeritageMaximumStages - 1, stage_index))
                return std::move(parser.result);
            auto& destination = parser.result.state.rng_states[index];
            destination.stage_index = static_cast<std::uint8_t>(stage_index);
            std::string stage_type;
            if (!parser.string(value, "stage_type", base + ".stage_type",
                               stage_type))
                return std::move(parser.result);
            if (stage_type == "quantization")
                destination.stage_type =
                    SampleHeritageRuntimeRngStageType::Quantization;
            else if (stage_type == "noise")
                destination.stage_type = SampleHeritageRuntimeRngStageType::Noise;
            else {
                parser.fail(SampleHeritageJsonStatus::InvalidEnum,
                            base + ".stage_type");
                return std::move(parser.result);
            }
            if (!parser.random_state(value, base + ".random_state",
                                     destination.random_state))
                return std::move(parser.result);
        }
        parser.result.runtime_status =
            validate_runtime_state_shape(parser.result.state);
        if (parser.result.runtime_status !=
            SampleHeritageRuntimeStateStatus::Ok) {
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.rng_states");
            return std::move(parser.result);
        }
        parser.result.status = SampleHeritageJsonStatus::Ok;
        parser.result.field_path.clear();
        return std::move(parser.result);
    } catch (const choc::value::Error& error) {
        parser.result.status =
            std::string_view(error.what()) ==
                    "This object already contains a member with the given name"
                ? SampleHeritageJsonStatus::DuplicateField
                : SampleHeritageJsonStatus::InvalidJson;
        parser.result.field_path = "$";
        return std::move(parser.result);
    } catch (const choc::json::ParseError&) {
        parser.result.status = SampleHeritageJsonStatus::InvalidJson;
        parser.result.field_path = "$";
        return std::move(parser.result);
    }
}

SampleHeritageRuntimeStateJsonWriteResult
write_sample_heritage_runtime_state_json(
    const SampleHeritageRuntimeState& state) {
    SampleHeritageRuntimeStateJsonWriteResult result;
    result.runtime_status = validate_runtime_state_shape(state);
    if (result.runtime_status != SampleHeritageRuntimeStateStatus::Ok)
        return result;

    result.json =
        "{\"schema_version\":1,\"profile_schema_version\":1,\"profile_id\":\"";
    result.json += state.bound_profile_id();
    result.json += "\",\"profile_digest_version\":1,\"profile_digest\":";
    append_digest_hex(result.json, state.profile_digest);
    result.json += ",\"rng_states\":[";
    for (std::size_t index = 0; index < state.rng_state_count; ++index) {
        if (index != 0) result.json.push_back(',');
        const auto& rng = state.rng_states[index];
        result.json += "{\"stage_index\":" + std::to_string(rng.stage_index);
        result.json += ",\"stage_type\":\"";
        result.json +=
            rng.stage_type == SampleHeritageRuntimeRngStageType::Quantization
                ? "quantization"
                : "noise";
        result.json += "\",\"random_state\":";
        append_seed(result.json, rng.random_state);
        result.json.push_back('}');
    }
    result.json += "]}";
    result.status = SampleHeritageJsonStatus::Ok;
    return result;
}

}  // namespace pulp::audio
