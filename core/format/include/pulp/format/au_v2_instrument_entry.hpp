#pragma once

// AU v2 instrument entry point generator
// For synths/samplers that receive MIDI and produce audio (aumu type)
//
// Usage (in one .cpp file per AU instrument plugin):
//   #include "my_synth.hpp"
//   #include <pulp/format/au_v2_instrument_entry.hpp>
//   PULP_AU_INSTRUMENT(MySynthAU, my_namespace::create_my_synth)

#include <pulp/format/au_v2_instrument.hpp>
#include <pulp/format/registry.hpp>
#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioUnitSDK/ComponentBase.h>

// Generate an AU v2 instrument entry point.
// ClassName determines the factory function name: ClassNameFactory
// factory_fn is the ProcessorFactory function
#define PULP_AU_INSTRUMENT(ClassName, factory_fn) \
    PULP_REGISTER_PLUGIN(factory_fn) \
    class ClassName : public pulp::format::au::PulpAUInstrument { \
    public: \
        explicit ClassName(AudioComponentInstance ci) : PulpAUInstrument(ci) {} \
    }; \
    AUSDK_COMPONENT_ENTRY(ausdk::AUMusicDeviceFactory, ClassName)
