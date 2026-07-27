#include "serialize_decode_support.hpp"

#include <bit>
#include <limits>

namespace pulp::timeline::detail {

runtime::Result<const JsonValue*, PersistenceError>
required_decode_member(const JsonValue& object_value, std::string_view name, std::string path) {
    if (object_value.kind != JsonValue::Kind::Object)
        return decode_fail<const JsonValue*>(PersistenceErrorCode::UnexpectedType, std::move(path),
                                             object_value.begin);
    const auto* value = object_value.find(name);
    if (!value)
        return decode_fail<const JsonValue*>(PersistenceErrorCode::MissingField,
                                             path + "/" + std::string(name), object_value.begin);
    return runtime::Ok(value);
}

runtime::Result<std::string, PersistenceError>
decode_string_field(const JsonValue& object_value, std::string_view name, std::string path) {
    auto value = required_decode_member(object_value, name, path);
    if (!value)
        return decode_fail<std::string>(value.error().code, value.error().path,
                                        value.error().byte_offset);
    if (value.value()->kind != JsonValue::Kind::String)
        return decode_fail<std::string>(PersistenceErrorCode::UnexpectedType,
                                        path + "/" + std::string(name), value.value()->begin);
    return runtime::Ok(value.value()->scalar);
}

runtime::Result<timebase::RationalRate, PersistenceError>
decode_rational_rate(const JsonValue& value, std::string path) {
    auto numerator = required_decode_member(value, "numerator", path);
    auto denominator = required_decode_member(value, "denominator", path);
    if (!numerator)
        return decode_fail<timebase::RationalRate>(numerator.error().code, numerator.error().path,
                                                   numerator.error().byte_offset);
    if (!denominator)
        return decode_fail<timebase::RationalRate>(
            denominator.error().code, denominator.error().path, denominator.error().byte_offset);
    auto n = parse_canonical_u64_string(*numerator.value(), path + "/numerator");
    auto d = parse_canonical_u64_string(*denominator.value(), path + "/denominator");
    if (!n)
        return decode_fail<timebase::RationalRate>(n.error().code, n.error().path,
                                                   n.error().byte_offset);
    if (!d)
        return decode_fail<timebase::RationalRate>(d.error().code, d.error().path,
                                                   d.error().byte_offset);
    const timebase::RationalRate rate{n.value(), d.value()};
    if (!rate.valid() || rate.normalized() != rate)
        return decode_fail<timebase::RationalRate>(PersistenceErrorCode::InvalidNumber,
                                                   std::move(path), value.begin);
    return runtime::Ok(rate);
}

runtime::Result<std::optional<TrackFreeze>, PersistenceError>
decode_track_freeze(const JsonValue* freeze, std::string path) {
    if (!freeze)
        return runtime::Ok(std::optional<TrackFreeze>{});
    const auto freeze_path = path + "/data/freeze";
    auto asset = required_decode_member(*freeze, "asset_id", freeze_path);
    auto frames = required_decode_member(*freeze, "frame_count", freeze_path);
    auto placement = required_decode_member(*freeze, "placement_start", freeze_path);
    auto render_plan = decode_string_field(*freeze, "render_plan_hash", freeze_path);
    auto rate = required_decode_member(*freeze, "sample_rate", freeze_path);
    auto source = required_decode_member(*freeze, "source_start", freeze_path);
    if (!asset || !frames || !placement || !render_plan || !rate || !source)
        return decode_fail<std::optional<TrackFreeze>>(PersistenceErrorCode::MissingField,
                                                       freeze_path);
    auto decoded_asset = parse_canonical_u64_string(*asset.value(), freeze_path + "/asset_id");
    auto decoded_frames = parse_canonical_u64_string(*frames.value(), freeze_path + "/frame_count");
    auto decoded_placement =
        parse_canonical_i64_string(*placement.value(), freeze_path + "/placement_start");
    auto decoded_rate = decode_rational_rate(*rate.value(), freeze_path + "/sample_rate");
    auto decoded_source =
        parse_canonical_i64_string(*source.value(), freeze_path + "/source_start");
    auto decoded_render_plan = ContentHash::from_hex(render_plan.value());
    if (!decoded_asset || !decoded_frames || !decoded_placement || !decoded_rate ||
        !decoded_source || !decoded_render_plan)
        return decode_fail<std::optional<TrackFreeze>>(PersistenceErrorCode::InvalidNumber,
                                                       freeze_path);
    return runtime::Ok(std::optional<TrackFreeze>{TrackFreeze{
        MediaRef{ItemId{decoded_asset.value()}, timebase::SamplePosition{decoded_source.value()},
                 decoded_frames.value()},
        timebase::SamplePosition{decoded_placement.value()}, decoded_rate.value(),
        *decoded_render_plan}});
}

runtime::Result<TrackMixer, PersistenceError> decode_track_mixer(const JsonValue* mixer,
                                                                 std::string path) {
    if (!mixer)
        return runtime::Ok(TrackMixer{});
    const auto mixer_path = path + "/data/mixer";
    auto gain = required_decode_member(*mixer, "gain_linear_bits", mixer_path);
    auto pan = required_decode_member(*mixer, "pan_bits", mixer_path);
    if (!gain || !pan)
        return decode_fail<TrackMixer>(PersistenceErrorCode::MissingField, mixer_path);
    auto gain_bits =
        parse_canonical_u64_string(*gain.value(), mixer_path + "/gain_linear_bits");
    auto pan_bits = parse_canonical_u64_string(*pan.value(), mixer_path + "/pan_bits");
    if (!gain_bits)
        return decode_fail<TrackMixer>(gain_bits.error().code, gain_bits.error().path,
                                       gain_bits.error().byte_offset);
    if (!pan_bits)
        return decode_fail<TrackMixer>(pan_bits.error().code, pan_bits.error().path,
                                       pan_bits.error().byte_offset);
    if (gain_bits.value() > std::numeric_limits<std::uint32_t>::max())
        return decode_fail<TrackMixer>(PersistenceErrorCode::InvalidNumber,
                                       mixer_path + "/gain_linear_bits");
    if (pan_bits.value() > std::numeric_limits<std::uint32_t>::max())
        return decode_fail<TrackMixer>(PersistenceErrorCode::InvalidNumber,
                                       mixer_path + "/pan_bits");
    // Range and NaN rejection belong to the model, which every decoded track
    // must pass through anyway; decoding only recovers the authored bits.
    return runtime::Ok(
        TrackMixer{std::bit_cast<float>(static_cast<std::uint32_t>(gain_bits.value())),
                   std::bit_cast<float>(static_cast<std::uint32_t>(pan_bits.value()))});
}

} // namespace pulp::timeline::detail
