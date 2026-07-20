#include <pulp/audio/sample_heritage_json.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace pulp::audio {
namespace {

using namespace std::string_view_literals;
using Value = choc::value::ValueView;

struct Parser {
    SampleHeritageTypedRuntimeStateJsonParseResult result;

    bool fail(SampleHeritageJsonStatus status, std::string path) {
        result.status = status;
        result.field_path = std::move(path);
        return false;
    }

    template<std::size_t N>
    bool audit(Value object, std::string_view path,
               const std::array<std::string_view, N>& fields) {
        if (!object.isObject())
            return fail(SampleHeritageJsonStatus::WrongType, std::string(path));
        for (std::uint32_t index = 0; index < object.size(); ++index) {
            const auto member = object.getObjectMemberAt(index);
            const auto known = std::find(fields.begin(), fields.end(), member.name);
            const auto member_path = std::string(path) + "." +
                                     std::string(member.name);
            if (known == fields.end())
                return fail(SampleHeritageJsonStatus::UnknownField, member_path);
            for (std::uint32_t earlier = 0; earlier < index; ++earlier) {
                if (object.getObjectMemberAt(earlier).name == member.name)
                    return fail(SampleHeritageJsonStatus::DuplicateField,
                                member_path);
            }
        }
        for (const auto field : fields) {
            if (!object.hasObjectMember(field))
                return fail(SampleHeritageJsonStatus::MissingField,
                            std::string(path) + "." + std::string(field));
        }
        return true;
    }

    template<typename Integer>
    bool integer(Value object, std::string_view field, std::string_view path,
                 std::uint64_t minimum, std::uint64_t maximum,
                 Integer& destination) {
        const auto value = object[field];
        if (!value.isInt())
            return fail(SampleHeritageJsonStatus::WrongType, std::string(path));
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0 ||
            static_cast<std::uint64_t>(signed_value) < minimum ||
            static_cast<std::uint64_t>(signed_value) > maximum)
            return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                        std::string(path));
        destination = static_cast<Integer>(signed_value);
        return true;
    }

    bool number(Value object, std::string_view field, std::string_view path,
                double minimum, double maximum, double& destination) {
        const auto value = object[field];
        if (!value.isInt() && !value.isFloat())
            return fail(SampleHeritageJsonStatus::WrongType, std::string(path));
        destination = value.isInt() ? static_cast<double>(value.get<std::int64_t>())
                                    : value.get<double>();
        if (!std::isfinite(destination) || destination < minimum ||
            destination > maximum)
            return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                        std::string(path));
        return true;
    }

    bool string(Value object, std::string_view field, std::string_view path,
                std::string& destination) {
        const auto value = object[field];
        if (!value.isString())
            return fail(SampleHeritageJsonStatus::WrongType, std::string(path));
        destination = value.getString();
        return true;
    }

    bool random_state(Value object, std::string_view path,
                      std::uint64_t& destination) {
        const auto value = object["random_state"];
        if (!value.isString())
            return fail(SampleHeritageJsonStatus::WrongType, std::string(path));
        const auto text = value.getString();
        if (text.empty() || text == "0" ||
            (text.size() > 1 && text.front() == '0'))
            return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                        std::string(path));
        const auto converted = std::from_chars(
            text.data(), text.data() + text.size(), destination);
        if (converted.ec != std::errc{} ||
            converted.ptr != text.data() + text.size())
            return fail(SampleHeritageJsonStatus::NumberOutOfRange,
                        std::string(path));
        return true;
    }
};

SampleHeritageRuntimeStateStatus validate_engine_state(
    const SampleHeritageRuntimeEngineState& state) noexcept {
    if (state.rng_state_count > state.rng_states.size())
        return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
    for (std::size_t index = 0; index < state.rng_state_count; ++index) {
        const auto& rng = state.rng_states[index];
        if (rng.stage_index >= kSampleHeritageMaximumStages ||
            (index != 0 &&
             state.rng_states[index - 1].stage_index >= rng.stage_index) ||
            (rng.stage_type != SampleHeritageRuntimeRngStageType::Quantization &&
             rng.stage_type != SampleHeritageRuntimeRngStageType::Noise &&
             rng.stage_type != SampleHeritageRuntimeRngStageType::LiveCyclic))
            return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
        if (rng.random_state == 0)
            return SampleHeritageRuntimeStateStatus::InvalidRandomState;
    }
    return SampleHeritageRuntimeStateStatus::Ok;
}

SampleHeritageRuntimeStateStatus validate_typed_state(
    const SampleHeritageTypedRuntimeState& state) noexcept {
    if (state.schema_version != kSampleHeritageTypedRuntimeStateSchemaVersion)
        return SampleHeritageRuntimeStateStatus::UnsupportedSchemaVersion;
    if (state.profile_schema_version != kSampleHeritageProfileSchemaVersion ||
        state.profile_digest_version != kSampleHeritageProfileDigestVersion ||
        !detail::valid_neutral_profile_id(state.bound_profile_id()))
        return SampleHeritageRuntimeStateStatus::ProfileMismatch;
    if (!std::isfinite(state.host_sample_rate) ||
        state.host_sample_rate < 8000.0 || state.host_sample_rate > 384000.0)
        return SampleHeritageRuntimeStateStatus::InvalidHostSampleRate;
    for (std::size_t index = 0; index < state.voice_states.size(); ++index) {
        if (state.voice_states[index].slot_index != index)
            return SampleHeritageRuntimeStateStatus::InvalidSlotLayout;
        const auto status = validate_engine_state(state.voice_states[index].engine);
        if (status != SampleHeritageRuntimeStateStatus::Ok) return status;
    }
    return validate_engine_state(state.bus_state);
}

bool parse_digest(std::string_view text,
                  std::array<std::uint8_t, 32>& destination) noexcept {
    if (text.size() != destination.size() * 2) return false;
    const auto nibble = [](char character) noexcept -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < destination.size(); ++index) {
        const auto high = nibble(text[index * 2]);
        const auto low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        destination[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

void append_digest(std::string& output,
                   const std::array<std::uint8_t, 32>& digest) {
    constexpr std::string_view digits = "0123456789abcdef";
    output.push_back('"');
    for (const auto byte : digest) {
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 0x0f]);
    }
    output.push_back('"');
}

void append_number(std::string& output, double value) {
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(),
                                         buffer.data() + buffer.size(), value,
                                         std::chars_format::general);
    output.append(buffer.data(), converted.ptr);
}

void append_random_state(std::string& output, std::uint64_t value) {
    std::array<char, 32> buffer{};
    const auto converted =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    output.push_back('"');
    output.append(buffer.data(), converted.ptr);
    output.push_back('"');
}

bool parse_engine_state(Parser& parser, Value object, std::string_view path,
                        SampleHeritageRuntimeEngineState& destination,
                        bool audit_object = true) {
    if (audit_object) {
        constexpr std::array fields{"rng_states"sv};
        if (!parser.audit(object, path, fields)) return false;
    }
    const auto states = object["rng_states"];
    if (!states.isArray())
        return parser.fail(SampleHeritageJsonStatus::WrongType,
                           std::string(path) + ".rng_states");
    if (states.size() > kSampleHeritageMaximumStages)
        return parser.fail(SampleHeritageJsonStatus::NumberOutOfRange,
                           std::string(path) + ".rng_states");
    destination.rng_state_count = states.size();
    for (std::uint32_t index = 0; index < states.size(); ++index) {
        const auto value = states[index];
        const auto base = std::string(path) + ".rng_states[" +
                          std::to_string(index) + "]";
        constexpr std::array state_fields{
            "stage_index"sv, "stage_type"sv, "random_state"sv};
        if (!parser.audit(value, base, state_fields)) return false;
        std::uint32_t stage_index = 0;
        if (!parser.integer(value, "stage_index", base + ".stage_index", 0,
                            kSampleHeritageMaximumStages - 1, stage_index))
            return false;
        auto& state = destination.rng_states[index];
        state.stage_index = static_cast<std::uint8_t>(stage_index);
        std::string stage_type;
        if (!parser.string(value, "stage_type", base + ".stage_type", stage_type))
            return false;
        if (stage_type == "quantization")
            state.stage_type = SampleHeritageRuntimeRngStageType::Quantization;
        else if (stage_type == "noise")
            state.stage_type = SampleHeritageRuntimeRngStageType::Noise;
        else if (stage_type == "live_cyclic")
            state.stage_type = SampleHeritageRuntimeRngStageType::LiveCyclic;
        else
            return parser.fail(SampleHeritageJsonStatus::InvalidEnum,
                               base + ".stage_type");
        if (!parser.random_state(value, base + ".random_state",
                                 state.random_state))
            return false;
    }
    const auto status = validate_engine_state(destination);
    if (status == SampleHeritageRuntimeStateStatus::Ok) return true;
    parser.result.runtime_status = status;
    return parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                       std::string(path) + ".rng_states");
}

void append_engine_state(std::string& output,
                         const SampleHeritageRuntimeEngineState& state) {
    output += "{\"rng_states\":[";
    for (std::size_t index = 0; index < state.rng_state_count; ++index) {
        if (index != 0) output.push_back(',');
        const auto& rng = state.rng_states[index];
        output += "{\"stage_index\":" + std::to_string(rng.stage_index);
        output += ",\"stage_type\":\"";
        output += rng.stage_type == SampleHeritageRuntimeRngStageType::Quantization
                      ? "quantization"
                      : (rng.stage_type == SampleHeritageRuntimeRngStageType::Noise
                             ? "noise"
                             : "live_cyclic");
        output += "\",\"random_state\":";
        append_random_state(output, rng.random_state);
        output.push_back('}');
    }
    output += "]}";
}

}  // namespace

SampleHeritageTypedRuntimeStateJsonParseResult
parse_sample_heritage_typed_runtime_state_json(std::string_view json) {
    Parser parser;
    try {
        const auto root_owner = choc::json::parse(json);
        const Value root = root_owner;
        if (!root.isObject()) {
            parser.fail(SampleHeritageJsonStatus::RootNotObject, "$");
            return std::move(parser.result);
        }
        std::uint32_t schema_version = 0;
        if (!root.hasObjectMember("schema_version")) {
            parser.fail(SampleHeritageJsonStatus::MissingField,
                        "$.schema_version");
            return std::move(parser.result);
        }
        if (!parser.integer(root, "schema_version", "$.schema_version", 0,
                            std::numeric_limits<std::uint32_t>::max(),
                            schema_version))
            return std::move(parser.result);
        parser.result.state.schema_version = schema_version;
        switch (schema_version) {
            case kSampleHeritageTypedRuntimeStateSchemaVersion:
                break;
            default:
                parser.result.runtime_status =
                    SampleHeritageRuntimeStateStatus::UnsupportedSchemaVersion;
                parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                            "$.schema_version");
                return std::move(parser.result);
        }
        constexpr std::array fields{
            "schema_version"sv, "profile_schema_version"sv, "profile_id"sv,
            "profile_digest_version"sv, "profile_digest"sv,
            "host_sample_rate"sv, "voice_states"sv, "bus_state"sv};
        if (!parser.audit(root, "$", fields)) return std::move(parser.result);

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
        std::string digest;
        if (!parser.string(root, "profile_digest", "$.profile_digest", digest))
            return std::move(parser.result);
        if (!parse_digest(digest, parser.result.state.profile_digest)) {
            parser.result.runtime_status =
                SampleHeritageRuntimeStateStatus::ProfileMismatch;
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                        "$.profile_digest");
            return std::move(parser.result);
        }
        if (!parser.number(root, "host_sample_rate", "$.host_sample_rate",
                           8000.0, 384000.0,
                           parser.result.state.host_sample_rate)) {
            parser.result.runtime_status =
                SampleHeritageRuntimeStateStatus::InvalidHostSampleRate;
            return std::move(parser.result);
        }

        const auto voices = root["voice_states"];
        if (!voices.isArray()) {
            parser.fail(SampleHeritageJsonStatus::WrongType, "$.voice_states");
            return std::move(parser.result);
        }
        if (voices.size() != kSampleHeritageRuntimeVoiceSlots) {
            parser.fail(SampleHeritageJsonStatus::NumberOutOfRange,
                        "$.voice_states");
            return std::move(parser.result);
        }
        for (std::uint32_t index = 0; index < voices.size(); ++index) {
            const auto value = voices[index];
            const auto base = "$.voice_states[" + std::to_string(index) + "]";
            constexpr std::array voice_fields{"slot_index"sv, "rng_states"sv};
            if (!parser.audit(value, base, voice_fields))
                return std::move(parser.result);
            std::uint32_t slot_index = 0;
            if (!parser.integer(value, "slot_index", base + ".slot_index", 0,
                                kSampleHeritageRuntimeVoiceSlots - 1,
                                slot_index))
                return std::move(parser.result);
            if (slot_index != index) {
                parser.result.runtime_status =
                    SampleHeritageRuntimeStateStatus::InvalidSlotLayout;
                parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed,
                            base + ".slot_index");
                return std::move(parser.result);
            }
            auto& destination = parser.result.state.voice_states[index];
            destination.slot_index = static_cast<std::uint8_t>(slot_index);
            if (!parse_engine_state(parser, value, base, destination.engine,
                                    false))
                return std::move(parser.result);
        }
        if (!parse_engine_state(parser, root["bus_state"], "$.bus_state",
                                parser.result.state.bus_state))
            return std::move(parser.result);

        parser.result.runtime_status = validate_typed_state(parser.result.state);
        if (parser.result.runtime_status != SampleHeritageRuntimeStateStatus::Ok) {
            parser.fail(SampleHeritageJsonStatus::ProfileValidationFailed, "$");
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
    } catch (...) {
        parser.result.status = SampleHeritageJsonStatus::InvalidJson;
        parser.result.field_path = "$";
        return std::move(parser.result);
    }
}

SampleHeritageTypedRuntimeStateJsonWriteResult
write_sample_heritage_typed_runtime_state_json(
    const SampleHeritageTypedRuntimeState& state) {
    SampleHeritageTypedRuntimeStateJsonWriteResult result;
    result.runtime_status = validate_typed_state(state);
    if (result.runtime_status != SampleHeritageRuntimeStateStatus::Ok)
        return result;

    result.json = "{\"schema_version\":" +
                  std::to_string(kSampleHeritageTypedRuntimeStateSchemaVersion) +
                  ",\"profile_schema_version\":" +
                  std::to_string(kSampleHeritageProfileSchemaVersion) +
                  ",\"profile_id\":\"";
    result.json += state.bound_profile_id();
    result.json += "\",\"profile_digest_version\":" +
                   std::to_string(kSampleHeritageProfileDigestVersion) +
                   ",\"profile_digest\":";
    append_digest(result.json, state.profile_digest);
    result.json += ",\"host_sample_rate\":";
    append_number(result.json, state.host_sample_rate);
    result.json += ",\"voice_states\":[";
    for (std::size_t index = 0; index < state.voice_states.size(); ++index) {
        if (index != 0) result.json.push_back(',');
        result.json += "{\"slot_index\":" + std::to_string(index) +
                       ",\"rng_states\":[";
        const auto& engine = state.voice_states[index].engine;
        for (std::size_t rng_index = 0; rng_index < engine.rng_state_count;
             ++rng_index) {
            if (rng_index != 0) result.json.push_back(',');
            const auto& rng = engine.rng_states[rng_index];
            result.json +=
                "{\"stage_index\":" + std::to_string(rng.stage_index) +
                ",\"stage_type\":\"";
            result.json +=
                rng.stage_type == SampleHeritageRuntimeRngStageType::Quantization
                    ? "quantization"
                    : (rng.stage_type == SampleHeritageRuntimeRngStageType::Noise
                           ? "noise"
                           : "live_cyclic");
            result.json += "\",\"random_state\":";
            append_random_state(result.json, rng.random_state);
            result.json.push_back('}');
        }
        result.json += "]}";
    }
    result.json += "],\"bus_state\":";
    append_engine_state(result.json, state.bus_state);
    result.json += "}";
    result.status = SampleHeritageJsonStatus::Ok;
    return result;
}

}  // namespace pulp::audio
