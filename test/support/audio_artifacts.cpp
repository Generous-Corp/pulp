// audio_artifacts.cpp — JSON metrics artifacts for failing render
// scenarios (harness PR 1B). See audio_artifacts.hpp for the schema
// contract.

#include "audio_artifacts.hpp"

#include <choc/text/choc_JSON.h>

#include <cctype>
#include <fstream>

namespace pulp::test::audio {

std::string metrics_to_json(const BufferMetrics& metrics,
                            std::string_view scenario) {
    auto channels = choc::value::createEmptyArray();
    for (const auto& ch : metrics.channels) {
        auto c = choc::value::createObject("ChannelMetrics");
        c.setMember("peak", ch.peak);
        c.setMember("peak_dbfs", ch.peak_dbfs());
        c.setMember("rms", ch.rms);
        c.setMember("rms_dbfs", ch.rms_dbfs());
        c.setMember("dc_offset", ch.dc_offset);
        c.setMember("nan_samples", static_cast<std::int64_t>(ch.nan_samples));
        c.setMember("inf_samples", static_cast<std::int64_t>(ch.inf_samples));
        c.setMember("clipped_samples",
                    static_cast<std::int64_t>(ch.clipped_samples));
        c.setMember("longest_silence_run",
                    static_cast<std::int64_t>(ch.longest_silence_run));
        channels.addArrayElement(c);
    }

    auto root = choc::value::createObject("AudioMetricsArtifact");
    root.setMember("schema_version", kMetricsArtifactSchemaVersion);
    root.setMember("scenario", std::string(scenario));
    root.setMember("sample_rate", metrics.sample_rate);
    root.setMember("frames", static_cast<std::int64_t>(metrics.num_frames));
    root.setMember("num_channels",
                   static_cast<std::int64_t>(metrics.num_channels));
    root.setMember("channels", channels);
    return choc::json::toString(root, /*useLineBreaks=*/true);
}

std::filesystem::path write_metrics_artifact(const BufferMetrics& metrics,
                                             std::string_view scenario) {
    std::string name;
    name.reserve(scenario.size());
    for (char c : scenario)
        name += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
                 c == '_' || c == '-')
                    ? c
                    : '-';
    if (name.empty())
        name = "scenario";

    const auto dir =
        std::filesystem::temp_directory_path() / "pulp-audio-metrics";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec); // best-effort; open() reports

    const auto path = dir / (name + ".json");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << metrics_to_json(metrics, scenario);
    return path;
}

} // namespace pulp::test::audio
