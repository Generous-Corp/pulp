// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

#include <pulp/audio/audio_file_layout_json.hpp>

#include <choc/text/choc_JSON.h>

#include <cstdint>
#include <limits>
#include <type_traits>
#include <variant>

namespace pulp::audio {
namespace {

// A size past 2^63 (only a hostile header can declare one) is emitted as a
// double rather than wrapping negative in the int64 the JSON value holds.
choc::value::Value json_number(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return choc::value::Value(static_cast<double>(value));
    return choc::value::Value(static_cast<std::int64_t>(value));
}

void set_field(choc::value::Value& object, const LayoutField& field) {
    std::visit(
        [&](const auto& value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::uint64_t>)
                object.setMember(field.name, json_number(value));
            else
                object.setMember(field.name, value);
        },
        field.value);
}

choc::value::Value regions_to_json(const AudioFileLayoutReport& report) {
    auto array = choc::value::createEmptyArray();
    for (const auto& region : report.regions) {
        auto object = choc::value::createObject("");
        object.setMember("offset", json_number(region.offset));
        object.setMember("declared_size", json_number(region.declared_size));
        object.setMember("actual_size", json_number(region.actual_size));
        object.setMember("id", region.id);
        object.setMember("kind", std::string(layout_region_kind_name(region.kind)));
        auto fields = choc::value::createObject("");
        for (const auto& field : region.fields)
            set_field(fields, field);
        object.setMember("fields", fields);
        if (!region.note.empty())
            object.setMember("note", region.note);
        array.addArrayElement(object);
    }
    return array;
}

choc::value::Value warnings_to_json(const AudioFileLayoutReport& report) {
    auto array = choc::value::createEmptyArray();
    for (const auto& warning : report.warnings) {
        auto object = choc::value::createObject("");
        object.setMember("kind", std::string(layout_warning_kind_name(warning.kind)));
        object.setMember("offset", json_number(warning.offset));
        object.setMember("detail", warning.detail);
        array.addArrayElement(object);
    }
    return array;
}

choc::value::Value stream_to_json(const LayoutStreamFacts& stream) {
    auto object = choc::value::createObject("");
    object.setMember("sample_rate", json_number(stream.sample_rate));
    object.setMember("channels", json_number(stream.channels));
    object.setMember("bits_per_sample", json_number(stream.bits_per_sample));
    object.setMember("valid_bits", json_number(stream.valid_bits));
    object.setMember("frames", json_number(stream.frames));
    object.setMember("duration_seconds", stream.duration_seconds);
    object.setMember("encoding", stream.encoding);
    object.setMember("bytes_per_frame", json_number(stream.bytes_per_frame));
    object.setMember("declared_frames", json_number(stream.declared_frames));
    return object;
}

choc::value::Value limits_to_json(const LayoutDecodeLimitsFact& fact) {
    auto object = choc::value::createObject("");
    object.setMember("exceeded", fact.exceeds());
    auto exceeded_by = choc::value::createEmptyArray();
    for (const auto name : fact.exceeded_names())
        exceeded_by.addArrayElement(std::string(name));
    object.setMember("exceeded_by", exceeded_by);
    auto limits = choc::value::createObject("");
    limits.setMember("max_frames", json_number(fact.limits.max_frames));
    limits.setMember("max_channels", json_number(fact.limits.max_channels));
    limits.setMember("max_output_bytes", json_number(fact.limits.max_output_bytes));
    object.setMember("limits", limits);
    object.setMember("output_bytes", json_number(fact.output_bytes));
    return object;
}

// `metadata.present` is the metadata regions by id and location; it is
// derived here rather than tracked separately so the two can never disagree.
choc::value::Value metadata_to_json(const AudioFileLayoutReport& report,
                                    const LayoutJsonOptions& options) {
    auto metadata = choc::value::createObject("");
    auto present = choc::value::createEmptyArray();
    for (const auto* region : report.metadata_regions()) {
        auto object = choc::value::createObject("");
        object.setMember("id", region->id);
        object.setMember("offset", json_number(region->offset));
        object.setMember("size", json_number(region->declared_size));
        present.addArrayElement(object);
    }
    metadata.setMember("present", present);
    if (options.include_metadata_text) {
        auto text = choc::value::createEmptyArray();
        for (const auto& entry : report.metadata_text) {
            auto object = choc::value::createObject("");
            object.setMember("region", entry.region_id);
            object.setMember("key", entry.key);
            object.setMember("text", entry.text);
            text.addArrayElement(object);
        }
        metadata.setMember("text", text);
    }
    return metadata;
}

} // namespace

std::string audio_file_layout_to_json(const AudioFileLayoutReport& report,
                                      const LayoutJsonOptions& options) {
    auto root = choc::value::createObject("");
    root.setMember("schema", std::string(kAudioFileInspectJsonSchema));

    auto input = choc::value::createObject("");
    input.setMember("name", options.input_name);
    input.setMember("size", json_number(report.file_size));
    root.setMember("input", input);

    root.setMember("container", std::string(audio_container_name(report.container)));
    root.setMember("container_detail", report.container_detail);
    if (report.profile)
        root.setMember("profile", std::string(wav_profile_name(*report.profile)));
    else
        root.setMember("profile", choc::value::Value());
    root.setMember("conformance", std::string(report.is_standard() ? "standard" : "violations"));
    root.setMember("facts_confidence", std::string(facts_confidence_name(report.facts_confidence)));
    root.setMember("bytes_read", json_number(report.bytes_read));

    root.setMember("stream", report.stream ? stream_to_json(*report.stream) : choc::value::Value());
    if (report.decode) {
        auto decode = choc::value::createObject("");
        decode.setMember("decodable", report.decode->decodable);
        decode.setMember("reason", report.decode->reason);
        decode.setMember("truncated", report.decode->truncated);
        root.setMember("decode", decode);
    } else {
        root.setMember("decode", choc::value::Value());
    }
    root.setMember("exceeds_decode_limits", report.decode_limits
                                                ? limits_to_json(*report.decode_limits)
                                                : choc::value::Value());

    root.setMember("regions", regions_to_json(report));
    root.setMember("regions_truncated", report.regions_truncated);
    root.setMember("warnings", warnings_to_json(report));
    root.setMember("metadata", metadata_to_json(report, options));

    if (report.truncated_tail_bytes)
        root.setMember("truncated_tail_bytes", json_number(*report.truncated_tail_bytes));
    if (!report.error.empty())
        root.setMember("error", report.error);
    return choc::json::toString(root, options.pretty);
}

} // namespace pulp::audio
