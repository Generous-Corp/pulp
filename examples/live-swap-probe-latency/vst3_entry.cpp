#include "src/latency_probe.hpp"
#include <pulp/format/vst3_entry.hpp>

// Probe-unique FUID (ASCII "PLpL" / "atPr" / "obe1" pattern — not registered).
static const Steinberg::FUID LatencyProbeUID(0x504C7050, 0x4C617450, 0x726F6265, 0x00000001);

PULP_VST3_PLUGIN(LatencyProbeUID, "Pulp Latency Probe",
                 Steinberg::Vst::PlugType::kFx, "Pulp", "0.1.0",
                 "local://latency-probe", probe::create_latency_probe)
