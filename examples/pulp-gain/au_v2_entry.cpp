// PulpGain AU v2 entry point
// Uses Apple AudioUnitSDK factory pattern for proper v2 hosting
// The generated factory function is PulpGainAUFactory (must match Info.plist)

#include "pulp_gain.hpp"
#include <pulp/format/registry.hpp>

// Include the adapter implementation directly (it defines the base class)
#include "../../core/format/src/au_v2_adapter.cpp"

#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioUnitSDK/ComponentBase.h>

// Register the Pulp processor factory at static init
PULP_REGISTER_PLUGIN(pulp::examples::create_pulp_gain)

// Concrete AU class with clean name for AUSDK_COMPONENT_ENTRY macro
// The macro generates "PulpGainAUFactory" as the factory function name
class PulpGainAU : public pulp::format::au::PulpAUEffect {
public:
    explicit PulpGainAU(AudioComponentInstance ci) : PulpAUEffect(ci) {}
};

AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, PulpGainAU)
