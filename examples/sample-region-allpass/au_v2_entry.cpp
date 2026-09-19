#include "allpass_processor.hpp"
#include <pulp/format/au_v2_entry.hpp>

// Packet F4 / PUB-04 AU v2 entry.  D4 owns the Processor and its bundle
// metadata; this translation unit only binds the shared factory to the AU
// adapter.  Keep the class name stable because it is the Info.plist factory.
PULP_AU_PLUGIN(SampleRegionAllpassAU, pulp::examples::create_sample_region_allpass)
