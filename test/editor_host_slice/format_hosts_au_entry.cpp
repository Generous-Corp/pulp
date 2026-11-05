// The slice processor's AU v2 entry for pulp-slice-formats-<kind>. Its factory
// function, PulpSliceFormatsAUFactory, is registered in process by the harness.

#include "gain_meter_processor.hpp"

#include <pulp/format/au_v2_entry.hpp>

PULP_AU_PLUGIN(PulpSliceFormatsAU, pulp::slice::GainMeterProcessor::create)
