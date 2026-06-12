#include <pulp/audio/audio_scope_json.hpp>

#include <pulp/audio/audio_probe_json.hpp>

#include <choc/text/choc_JSON.h>

namespace pulp::audio {
namespace {

void set_optional_number(choc::value::Value& object, const char* key,
                         bool available, double value) {
    if (available) {
        object.setMember(key, value);
    } else {
        object.setMember(key, choc::value::Value());
    }
}

choc::value::Value warnings_to_json(const std::vector<std::string>& warnings) {
    auto array = choc::value::createEmptyArray();
    for (const auto& warning : warnings)
        array.addArrayElement(warning);
    return array;
}

}  // namespace

std::string audio_scope_result_to_json(const AudioScopeResult& result,
                                       bool pretty) {
    auto root = choc::value::createObject("");
    root.setMember("schema", std::string(kAudioScopeJsonSchema));
    root.setMember("version", static_cast<std::int64_t>(kAudioScopeJsonVersion));

    auto source = choc::value::createObject("source");
    source.setMember("kind", result.source_kind.empty()
        ? std::string("live_probe")
        : result.source_kind);
    if (!result.source_path.empty())
        source.setMember("path", result.source_path);
    source.setMember("stage", std::string(audio_probe_stage_name(result.stage)));
    source.setMember("sample_rate", result.acquisition.sample_rate);
    source.setMember("channel_count",
                     static_cast<std::int64_t>(result.acquisition.source_channel_count));
    source.setMember("selected_channel",
                     static_cast<std::int64_t>(result.acquisition.selected_channel));
    source.setMember("sequence_number",
                     static_cast<std::int64_t>(
                         result.acquisition.source_sequence_number));
    root.setMember("source", source);

    auto acquisition = choc::value::createObject("acquisition");
    acquisition.setMember("window_samples",
                          static_cast<std::int64_t>(
                              result.acquisition.window_samples));
    acquisition.setMember("source_frames",
                          static_cast<std::int64_t>(
                              result.acquisition.source_frames));
    acquisition.setMember("window_start",
                          static_cast<std::int64_t>(
                              result.acquisition.window_start));
    acquisition.setMember("trigger_mode",
                          std::string(audio_scope_trigger_mode_name(
                              result.trigger_mode)));
    acquisition.setMember("trigger_found", result.acquisition.trigger_found);
    acquisition.setMember("trigger_sample",
                          static_cast<std::int64_t>(
                              result.acquisition.trigger_sample));
    root.setMember("acquisition", acquisition);

    auto measurements = choc::value::createObject("measurements");
    set_optional_number(measurements, "peak_to_peak",
                        result.measurements.peak_to_peak_available,
                        result.measurements.peak_to_peak);
    set_optional_number(measurements, "rms", result.measurements.rms_available,
                        result.measurements.rms);
    set_optional_number(measurements, "dc_offset",
                        result.measurements.dc_offset_available,
                        result.measurements.dc_offset);
    set_optional_number(measurements, "crest_factor",
                        result.measurements.crest_factor_available,
                        result.measurements.crest_factor);
    set_optional_number(measurements, "frequency_hz",
                        result.measurements.frequency_available,
                        result.measurements.frequency_hz);
    set_optional_number(measurements, "period_samples",
                        result.measurements.frequency_available,
                        result.measurements.period_samples);
    root.setMember("measurements", measurements);

    std::vector<std::string> warnings = result.acquisition.warnings;
    warnings.insert(warnings.end(), result.measurements.warnings.begin(),
                    result.measurements.warnings.end());
    root.setMember("warnings", warnings_to_json(warnings));

    return choc::json::toString(root, pretty);
}

}  // namespace pulp::audio
