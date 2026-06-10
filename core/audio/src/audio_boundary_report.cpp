#include <pulp/audio/audio_boundary_report.hpp>

namespace pulp::audio {
namespace {

bool snapshot_has_signal(const AudioProbeSnapshot& s, float silence_threshold) {
    // "Signal" means the most recent block was not in a silence run AND the
    // observed aggregate peak exceeds the silence threshold.
    return s.silence_run_blocks == 0 && s.peak_max > silence_threshold;
}

void append_stage_line(std::string& out,
                       const char* label,
                       const std::optional<AudioProbeSnapshot>& snap,
                       float silence_threshold) {
    out += label;
    if (!snap.has_value()) {
        out += ": no probe\n";
        return;
    }
    const AudioProbeSnapshot& s = *snap;
    if (snapshot_has_signal(s, silence_threshold)) {
        out += ": nonzero, peak_max ";
    } else {
        out += ": silent (";
        out += std::to_string(s.silence_run_blocks);
        out += " blocks), peak_max ";
    }
    out += std::to_string(s.peak_max);
    out += ", clip ";
    out += std::to_string(s.clip_count);
    out += ", nan/inf ";
    out += std::to_string(s.nan_inf_count);
    out += "\n";
}

}  // namespace

BoundaryReport build_boundary_report(const BoundaryReportInputs& inputs) {
    BoundaryReport report;
    std::string& out = report.text;

    append_stage_line(out, "Processor output", inputs.processor_output,
                      inputs.silence_threshold);
    append_stage_line(out, "Standalone boundary", inputs.standalone_boundary,
                      inputs.silence_threshold);

    if (inputs.device.has_value()) {
        const DeviceStatsView& d = *inputs.device;
        out += "Device: ";
        out += d.callback_running ? "callback running, " : "callback NOT running, ";
        out += std::to_string(static_cast<long long>(d.sample_rate));
        out += " Hz / ";
        out += std::to_string(d.buffer_size);
        out += " frames, ";
        out += std::to_string(d.xruns);
        out += " xruns, ";
        out += std::to_string(d.cpu_overloads);
        out += " overloads\n";
    } else {
        out += "Device: no stats\n";
    }

    // Diagnosis precedence: pick the earliest boundary that explains the
    // missing/odd audio so the next code search is obvious.
    const bool have_any =
        inputs.processor_output.has_value() ||
        inputs.standalone_boundary.has_value();
    if (!have_any) {
        report.diagnosis = BoundaryDiagnosis::kNoProbeData;
        out += "Likely issue: no probe data captured\n";
        return report;
    }

    const bool proc_signal =
        inputs.processor_output.has_value() &&
        snapshot_has_signal(*inputs.processor_output, inputs.silence_threshold);
    const bool boundary_signal =
        inputs.standalone_boundary.has_value() &&
        snapshot_has_signal(*inputs.standalone_boundary, inputs.silence_threshold);

    if (inputs.processor_output.has_value() && !proc_signal) {
        report.diagnosis = BoundaryDiagnosis::kProcessorSilent;
        out += "Likely issue: processor rendered silence\n";
        return report;
    }

    if (inputs.standalone_boundary.has_value() && proc_signal && !boundary_signal) {
        report.diagnosis = BoundaryDiagnosis::kStandaloneBoundarySilent;
        out += "Likely issue: output buffer not copied after processor render\n";
        return report;
    }

    // Signal reached the observed boundary. If the device shows trouble, flag it.
    if (inputs.device.has_value()) {
        const DeviceStatsView& d = *inputs.device;
        if (!d.callback_running || d.xruns > 0) {
            report.diagnosis = BoundaryDiagnosis::kDeviceProblem;
            out += "Likely issue: device path (callback stopped or xruns)\n";
            return report;
        }
    }

    report.diagnosis = BoundaryDiagnosis::kSignalPresent;
    out += "Likely issue: none — signal present at observed boundary\n";
    return report;
}

}  // namespace pulp::audio
