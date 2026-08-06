#include "serialize_modulation_decode.hpp"

#include "bounded_increment.hpp"
#include "serialize_automation_decode.hpp"

#include <bit>
#include <limits>

namespace pulp::timeline::detail {
namespace {

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code, std::string path,
                                          std::size_t offset = 0, std::uint64_t actual = 0,
                                          std::uint64_t limit = 0) {
    return runtime::Err(PersistenceError{code, offset, actual, limit, std::move(path)});
}

runtime::Result<const JsonValue*, PersistenceError> require_member(const JsonValue& value,
                                                                   std::string_view name,
                                                                   JsonValue::Kind kind,
                                                                   const std::string& path) {
    const auto* found = value.kind == JsonValue::Kind::Object ? value.find(name) : nullptr;
    if (!found)
        return fail<const JsonValue*>(PersistenceErrorCode::MissingField,
                                      path + "/" + std::string(name), value.begin);
    if (found->kind != kind)
        return fail<const JsonValue*>(PersistenceErrorCode::UnexpectedType,
                                      path + "/" + std::string(name), found->begin);
    return runtime::Ok(found);
}

// A float is persisted as its canonical bit pattern so a value survives a round
// trip exactly, including the negative zero and subnormals a decimal spelling
// would round away.
runtime::Result<float, PersistenceError> decode_float_bits(const JsonValue& value,
                                                           const std::string& path) {
    auto bits = parse_canonical_u64_string(value, path);
    if (!bits)
        return runtime::Err(bits.error());
    if (bits.value() > std::numeric_limits<std::uint32_t>::max())
        return fail<float>(PersistenceErrorCode::InvalidSchema, path, value.begin);
    return runtime::Ok(std::bit_cast<float>(static_cast<std::uint32_t>(bits.value())));
}

template <typename T, typename Decode>
runtime::Result<std::vector<T>, PersistenceError>
decode_governed_array(const JsonValue& value, std::size_t& count, std::size_t limit,
                      std::string path, Decode&& decode) {
    if (value.kind != JsonValue::Kind::Array)
        return fail<std::vector<T>>(PersistenceErrorCode::UnexpectedType, std::move(path),
                                    value.begin);
    std::vector<T> decoded;
    decoded.reserve(value.array.size());
    for (std::size_t index = 0; index < value.array.size(); ++index) {
        auto element_path = path + "/" + std::to_string(index);
        const auto increment = bounded_increment(count, limit);
        if (!increment)
            return fail<std::vector<T>>(PersistenceErrorCode::LimitExceeded,
                                        std::move(element_path), value.array[index].begin,
                                        increment.actual, limit);
        auto element = decode(value.array[index], element_path);
        if (!element)
            return runtime::Err(element.error());
        decoded.push_back(std::move(element).value());
    }
    return runtime::Ok(std::move(decoded));
}

} // namespace

runtime::Result<std::vector<Modulator>, PersistenceError>
decode_modulators(const JsonValue& value, const DecodeLimits& limits, std::size_t& count,
                  std::string path) {
    return decode_governed_array<Modulator>(
        value, count, limits.max_modulators, std::move(path),
        [](const JsonValue& element,
           const std::string& element_path) -> runtime::Result<Modulator, PersistenceError> {
            auto data =
                validate_exact_envelope(element, "pulp.timeline.modulator", 1, element_path);
            if (!data)
                return runtime::Err(data.error());
            const auto data_path = element_path + "/data";
            auto id = require_member(*data.value(), "id", JsonValue::Kind::String, data_path);
            auto kind = require_member(*data.value(), "kind", JsonValue::Kind::String, data_path);
            auto name = require_member(*data.value(), "name", JsonValue::Kind::String, data_path);
            if (!id)
                return runtime::Err(id.error());
            if (!kind)
                return runtime::Err(kind.error());
            if (!name)
                return runtime::Err(name.error());
            auto decoded_id = parse_canonical_u64_string(*id.value(), data_path + "/id");
            if (!decoded_id)
                return runtime::Err(decoded_id.error());
            const auto decoded_kind = modulator_kind_from_name(kind.value()->scalar);
            if (!decoded_kind)
                return fail<Modulator>(PersistenceErrorCode::InvalidSchema, data_path + "/kind",
                                       kind.value()->begin);
            return runtime::Ok(
                Modulator{{decoded_id.value()}, *decoded_kind, name.value()->scalar});
        });
}

runtime::Result<std::vector<MacroControl>, PersistenceError>
decode_macro_controls(const JsonValue& value, const DecodeLimits& limits, std::size_t& count,
                      std::string path) {
    return decode_governed_array<MacroControl>(
        value, count, limits.max_macro_controls, std::move(path),
        [](const JsonValue& element,
           const std::string& element_path) -> runtime::Result<MacroControl, PersistenceError> {
            auto data =
                validate_exact_envelope(element, "pulp.timeline.macro_control", 1, element_path);
            if (!data)
                return runtime::Err(data.error());
            const auto data_path = element_path + "/data";
            auto id = require_member(*data.value(), "id", JsonValue::Kind::String, data_path);
            auto name = require_member(*data.value(), "name", JsonValue::Kind::String, data_path);
            auto bits =
                require_member(*data.value(), "value_bits", JsonValue::Kind::String, data_path);
            if (!id)
                return runtime::Err(id.error());
            if (!name)
                return runtime::Err(name.error());
            if (!bits)
                return runtime::Err(bits.error());
            auto decoded_id = parse_canonical_u64_string(*id.value(), data_path + "/id");
            if (!decoded_id)
                return runtime::Err(decoded_id.error());
            auto decoded_value = decode_float_bits(*bits.value(), data_path + "/value_bits");
            if (!decoded_value)
                return runtime::Err(decoded_value.error());
            return runtime::Ok(
                MacroControl{{decoded_id.value()}, name.value()->scalar, decoded_value.value()});
        });
}

runtime::Result<std::vector<ModulationRoute>, PersistenceError>
decode_modulation_routes(const JsonValue& value, const DecodeLimits& limits, std::size_t& count,
                         std::string path) {
    return decode_governed_array<ModulationRoute>(
        value, count, limits.max_modulation_routes, std::move(path),
        [](const JsonValue& element,
           const std::string& element_path) -> runtime::Result<ModulationRoute, PersistenceError> {
            auto data = validate_exact_envelope(element, "pulp.timeline.modulation_route", 1,
                                                element_path);
            if (!data)
                return runtime::Err(data.error());
            const auto data_path = element_path + "/data";
            auto depth =
                require_member(*data.value(), "depth_bits", JsonValue::Kind::String, data_path);
            auto enabled =
                require_member(*data.value(), "enabled", JsonValue::Kind::Boolean, data_path);
            auto id = require_member(*data.value(), "id", JsonValue::Kind::String, data_path);
            auto source =
                require_member(*data.value(), "source_id", JsonValue::Kind::String, data_path);
            auto source_kind =
                require_member(*data.value(), "source_kind", JsonValue::Kind::String, data_path);
            auto target =
                require_member(*data.value(), "target", JsonValue::Kind::Object, data_path);
            if (!depth)
                return runtime::Err(depth.error());
            if (!enabled)
                return runtime::Err(enabled.error());
            if (!id)
                return runtime::Err(id.error());
            if (!source)
                return runtime::Err(source.error());
            if (!source_kind)
                return runtime::Err(source_kind.error());
            if (!target)
                return runtime::Err(target.error());
            auto decoded_id = parse_canonical_u64_string(*id.value(), data_path + "/id");
            if (!decoded_id)
                return runtime::Err(decoded_id.error());
            auto decoded_source =
                parse_canonical_u64_string(*source.value(), data_path + "/source_id");
            if (!decoded_source)
                return runtime::Err(decoded_source.error());
            const auto decoded_source_kind =
                modulation_source_kind_from_name(source_kind.value()->scalar);
            if (!decoded_source_kind)
                return fail<ModulationRoute>(PersistenceErrorCode::InvalidSchema,
                                             data_path + "/source_kind",
                                             source_kind.value()->begin);
            auto decoded_depth = decode_float_bits(*depth.value(), data_path + "/depth_bits");
            if (!decoded_depth)
                return runtime::Err(decoded_depth.error());
            auto decoded_target =
                decode_parameter_target(*target.value(), data_path + "/target");
            if (!decoded_target)
                return runtime::Err(decoded_target.error());
            return runtime::Ok(ModulationRoute{{decoded_id.value()},
                                               {{decoded_source.value()}, *decoded_source_kind},
                                               std::move(decoded_target).value(),
                                               decoded_depth.value(),
                                               enabled.value()->boolean});
        });
}

} // namespace pulp::timeline::detail
