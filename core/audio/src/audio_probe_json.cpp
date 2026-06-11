// audio_probe_json.cpp — snapshot → JSON for the live Audio Inspector dump.
//
// See audio_probe_json.hpp for the schema contract. CHOC-first (choc::value /
// choc::json), mirroring the offline Audio Doctor's audio_doctor_artifacts.cpp
// style. NON-RT only — runs after the audio callback has published a snapshot.

#include <pulp/audio/audio_probe_json.hpp>

#include <choc/text/choc_JSON.h>

#include <cmath>

namespace pulp::audio {

std::string_view audio_probe_stage_name(AudioProbeStage stage) {
    switch (stage) {
        case AudioProbeStage::kProcessorOutput:           return "processor_output";
        case AudioProbeStage::kStandaloneOutputBoundary:  return "standalone_output_boundary";
        case AudioProbeStage::kMeterBridge:               return "meter_bridge";
        case AudioProbeStage::kDeviceCallback:            return "device_callback";
        case AudioProbeStage::kGraphNode:                 return "graph_node";
        case AudioProbeStage::kUnknown:                   break;
    }
    return "unknown";
}

namespace {

// 20*log10(linear). A linear value of 0 has no finite dBFS; the JSON object
// records `null` so a reader can tell "true silence" from a finite low level.
void set_dbfs_member(choc::value::Value& root, std::string_view key, float linear) {
    if (linear > 0.0f) {
        root.setMember(std::string(key),
                       20.0 * std::log10(static_cast<double>(linear)));
    } else {
        root.setMember(std::string(key), choc::value::Value());  // JSON null
    }
}

}  // namespace

std::string audio_probe_snapshot_to_json(const AudioProbeSnapshot& snapshot,
                                         const AudioStats& stats,
                                         bool pretty) {
    auto root = choc::value::createObject("");

    // Identity tuple.
    root.setMember("stage", std::string(audio_probe_stage_name(snapshot.stage_id)));
    root.setMember("sample_rate", snapshot.sample_rate);
    root.setMember("block_size", static_cast<std::int64_t>(snapshot.block_size));
    root.setMember("channel_count", static_cast<std::int64_t>(snapshot.channel_count));
    root.setMember("sequence_number", static_cast<std::int64_t>(snapshot.sequence_number));

    // Aggregate level (linear + dBFS).
    root.setMember("peak_max", static_cast<double>(snapshot.peak_max));
    root.setMember("rms_max", static_cast<double>(snapshot.rms_max));
    set_dbfs_member(root, "peak_dbfs", snapshot.peak_max);
    set_dbfs_member(root, "rms_dbfs", snapshot.rms_max);

    // Content-event counters (from the snapshot).
    root.setMember("clip_count", static_cast<std::int64_t>(snapshot.clip_count));
    root.setMember("nan_inf_count", static_cast<std::int64_t>(snapshot.nan_inf_count));
    root.setMember("clipped_blocks", static_cast<std::int64_t>(snapshot.clipped_blocks));
    root.setMember("nan_blocks", static_cast<std::int64_t>(snapshot.nan_blocks));
    root.setMember("silence_run_blocks", static_cast<std::int64_t>(snapshot.silence_run_blocks));
    root.setMember("callbacks", static_cast<std::int64_t>(snapshot.callbacks));

    // Release-safe counter subset (AudioStats). The device-owned mirrors
    // (device_xruns / cpu_overloads) round out the "no audio" picture.
    root.setMember("underruns", static_cast<std::int64_t>(stats.underruns));
    root.setMember("device_xruns", static_cast<std::int64_t>(stats.device_xruns));
    root.setMember("cpu_overloads", static_cast<std::int64_t>(stats.cpu_overloads));

    return choc::json::toString(root, pretty);
}

}  // namespace pulp::audio
