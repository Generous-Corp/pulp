// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

#include <pulp/audio/audio_file_layout_json.hpp>

#include <choc/text/choc_JSON.h>

#include <cstdint>
#include <limits>

namespace pulp::audio {
namespace {

// JSON numbers here are int64; a size past 2^63 (only a hostile header can
// declare one) is emitted as a double rather than wrapping negative.
choc::value::Value as_i64(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return choc::value::Value(static_cast<double>(value));
    return choc::value::Value(static_cast<std::int64_t>(value));
}

void set_field(choc::value::Value& object, const LayoutField& field) {
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::uint64_t>)
                object.setMember(field.name, as_i64(value));
            else if constexpr (std::is_same_v<T, std::int64_t>)
                object.setMember(field.name, value);
            else
                object.setMember(field.name, value);
        },
        field.value);
}

choc::value::Value regions_to_json(const AudioFileLayoutReport& report) {
    auto array = choc::value::createEmptyArray();
    for (const auto& region : report.regions) {
        auto object = choc::value::createObject("");
        object.setMember("offset", as_i64(region.offset));
        object.setMember("declared_size", as_i64(region.declared_size));
        object.setMember("actual_size", as_i64(region.actual_size));
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
        object.setMember("offset", as_i64(warning.offset));
        object.setMember("detail", warning.detail);
        array.addArrayElement(object);
    }
    return array;
}

choc::value::Value stream_to_json(const LayoutStreamFacts& stream) {
    auto object = choc::value::createObject("");
    object.setMember("sample_rate", as_i64(stream.sample_rate));
    object.setMember("channels", as_i64(stream.channels));
    object.setMember("bits_per_sample", as_i64(stream.bits_per_sample));
    object.setMember("valid_bits", as_i64(stream.valid_bits));
    object.setMember("frames", as_i64(stream.frames));
    object.setMember("duration_seconds", stream.duration_seconds);
    object.setMember("encoding", stream.encoding);
    object.setMember("bytes_per_frame", as_i64(stream.bytes_per_frame));
    return object;
}

choc::value::Value limits_to_json(const LayoutDecodeLimitsFact& fact) {
    auto object = choc::value::createObject("");
    object.setMember("exceeded", fact.exceeds);
    auto exceeded_by = choc::value::createEmptyArray();
    for (const auto& name : fact.exceeded)
        exceeded_by.addArrayElement(name);
    object.setMember("exceeded_by", exceeded_by);
    auto limits = choc::value::createObject("");
    limits.setMember("max_frames", as_i64(fact.limits.max_frames));
    limits.setMember("max_channels", as_i64(fact.limits.max_channels));
    limits.setMember("max_output_bytes", as_i64(fact.limits.max_output_bytes));
    object.setMember("limits", limits);
    object.setMember("output_bytes", as_i64(fact.output_bytes));
    return object;
}

} // namespace

std::string audio_file_layout_to_json(const AudioFileLayoutReport& report,
                                      const LayoutJsonOptions& options) {
    auto root = choc::value::createObject("");
    root.setMember("schema", std::string(kAudioFileInspectJsonSchema));

    auto input = choc::value::createObject("");
    input.setMember("name", options.input_name);
    input.setMember("size", as_i64(report.file_size));
    root.setMember("input", input);

    root.setMember("container", std::string(audio_container_name(report.container)));
    root.setMember("container_detail", report.container_detail);
    if (report.profile)
        root.setMember("profile", std::string(wav_profile_name(*report.profile)));
    else
        root.setMember("profile", choc::value::Value());
    root.setMember("conformance", std::string(report.is_standard() ? "standard" : "violations"));
    root.setMember("facts_confidence", std::string(facts_confidence_name(report.facts_confidence)));
    root.setMember("bytes_read", as_i64(report.bytes_read));

    root.setMember("stream", report.stream ? stream_to_json(*report.stream) : choc::value::Value());
    if (report.decode) {
        auto decode = choc::value::createObject("");
        decode.setMember("decodable", report.decode->decodable);
        decode.setMember("reason", report.decode->reason);
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

    auto metadata = choc::value::createObject("");
    auto present = choc::value::createEmptyArray();
    for (const auto& entry : report.metadata_present) {
        auto object = choc::value::createObject("");
        object.setMember("id", entry.id);
        object.setMember("offset", as_i64(entry.offset));
        object.setMember("size", as_i64(entry.size));
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
    root.setMember("metadata", metadata);

    if (report.truncated_tail_bytes)
        root.setMember("truncated_tail_bytes", as_i64(*report.truncated_tail_bytes));
    if (!report.error.empty())
        root.setMember("error", report.error);
    return choc::json::toString(root, options.pretty);
}

} // namespace pulp::audio
