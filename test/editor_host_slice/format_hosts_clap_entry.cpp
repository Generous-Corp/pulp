// The slice processor's CLAP entry for pulp-slice-formats-<kind>: a view-free
// build (no PULP_CLAP_GUI), so its only GUI is the editor-host contract's.

#include "gain_meter_processor.hpp"

#include <pulp/format/clap_entry.hpp>

PULP_CLAP_PLUGIN(pulp::slice::GainMeterProcessor::create)
