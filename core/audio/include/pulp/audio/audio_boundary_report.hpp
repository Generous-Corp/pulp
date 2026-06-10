#pragma once

/// @file audio_boundary_report.hpp
/// Complaint-oriented "no sound" / "sounds wrong" boundary report.
///
/// This is a NON-RT consumer-side helper. It takes already-published probe
/// snapshots (the RT producer side) plus device counters and produces the
/// stage-by-stage diagnosis the harness plan asks for — distinguishing
/// processor-output silence, standalone-boundary silence, and device-xrun
/// problems (harness plan Use-Case 3, "Diagnose No Audio"). It does no audio
/// analysis itself; it classifies the scalar facts already in each snapshot.
///
/// The full multi-stage chain (processor output → standalone boundary → meter
/// bridge → device callback) lands incrementally. This first version models
/// the stages that exist today and the device summary, and leaves the others
/// as explicitly "no probe" rather than faking zeros.

#include <pulp/audio/audio_probe_snapshot.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace pulp::audio {

/// Device-side facts the report mirrors (owned by AudioDeviceManager / the
/// CoreAudio overload listener — never by the probe).
struct DeviceStatsView {
    bool callback_running = false;
    double sample_rate = 0.0;
    std::uint32_t buffer_size = 0;
    std::uint64_t xruns = 0;
    std::uint64_t cpu_overloads = 0;
};

/// Coarse classification of where audio was lost (or that it was fine).
enum class BoundaryDiagnosis {
    kNoProbeData,            ///< No usable probe snapshot was provided.
    kSignalPresent,          ///< Audio is flowing at the observed boundary.
    kProcessorSilent,        ///< Processor-output stage was silent.
    kStandaloneBoundarySilent,  ///< Processor had signal but the boundary was silent.
    kDeviceProblem,          ///< Signal present but device shows xruns/not running.
};

struct BoundaryReport {
    BoundaryDiagnosis diagnosis = BoundaryDiagnosis::kNoProbeData;
    /// Human-readable, stage-by-stage text (the plan's "no audio" report).
    std::string text;
};

/// Inputs to the report. Each stage snapshot is optional — a missing stage is
/// reported honestly as "no probe", not as silence.
struct BoundaryReportInputs {
    std::optional<AudioProbeSnapshot> processor_output;
    std::optional<AudioProbeSnapshot> standalone_boundary;
    std::optional<DeviceStatsView> device;
    /// A block whose peak is at or below this (linear) counts as silent.
    float silence_threshold = 3.1622776e-5f;  // ~ -90 dBFS
};

/// Build the complaint-oriented boundary report from the given inputs.
BoundaryReport build_boundary_report(const BoundaryReportInputs& inputs);

}  // namespace pulp::audio
