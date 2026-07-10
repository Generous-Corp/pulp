#include "cv_osc_processor.hpp"
#include <pulp/format/au_v2_entry.hpp>

// The class name is load-bearing: AUSDK_COMPONENT_ENTRY exports
// `<ClassName>Factory`, and the bundle's exported-symbol list names
// `_<cmake-target>AUFactory`. ClassName must therefore be exactly the CMake
// target (`BrewCvOsc`) with an `AU` suffix, matching case.
PULP_AU_PLUGIN(BrewCvOscAU, pulp::examples::brew::create_cv_osc)
