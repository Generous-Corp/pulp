#include "allpass_processor.hpp"
#include <pulp/format/clap_entry.hpp>

// Packet F4 / PUB-04 CLAP entry.  The descriptor (including bundle_id and the
// single parameter 2901) comes from D4's SampleRegionAllpassProcessor.
PULP_CLAP_PLUGIN(pulp::examples::create_sample_region_allpass)
