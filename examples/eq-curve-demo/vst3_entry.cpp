// EqCurveDemo VST3 entry point.
#include "eq_curve_demo.hpp"
#include <pulp/format/vst3_entry.hpp>

// Unique ID — stable across versions, never change.
static const Steinberg::FUID EqCurveDemoUID(0x50554C50, 0x45514300, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(EqCurveDemoUID, "EQ Curve Demo", Steinberg::Vst::PlugType::kFx,
                 "Pulp Examples", "0.1.0", "https://github.com/danielraffel/pulp",
                 pulp::examples::create_eq_curve_demo)
