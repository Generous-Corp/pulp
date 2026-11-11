#include "scale_probe.hpp"

#include <pulp/format/au_v2_instrument_entry.hpp>

// An instrument (aumu): CATEGORY Instrument sets the type, and the instrument
// entry gives it the MusicDevice base with its output elements.
PULP_AU_INSTRUMENT(PulpScaleProbeAU, pulp::test_fixtures::create_scale_probe)
