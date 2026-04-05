#pragma once

// Generic AU v2 entry point generator
// Plugin developers include this and call PULP_AU_PLUGIN() with their factory
// function and a class name. All AudioUnitSDK boilerplate is generated.
//
// Usage (in one .cpp file per AU plugin):
//   #include "my_processor.hpp"
//   #include <pulp/format/au_v2_entry.hpp>
//
//   // The class name determines the factory function: MyPluginAUFactory
//   // This must match the factoryFunction in Info.plist.au
//   PULP_AU_PLUGIN(MyPluginAU, my_namespace::create_my_processor)
//
// This generates:
//   - A class MyPluginAU : PulpAUEffect (the AudioUnit implementation)
//   - A factory function MyPluginAUFactory (for the Info.plist factoryFunction)
//   - Plugin registration via PULP_REGISTER_PLUGIN

#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/registry.hpp>
#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioUnitSDK/ComponentBase.h>

// Generate an AU v2 entry point.
// ClassName determines the factory function name: ClassNameFactory
// factory_fn is the ProcessorFactory function that creates the Processor
#define PULP_AU_PLUGIN(ClassName, factory_fn) \
    PULP_REGISTER_PLUGIN(factory_fn) \
    class ClassName : public pulp::format::au::PulpAUEffect { \
    public: \
        explicit ClassName(AudioComponentInstance ci) : PulpAUEffect(ci) {} \
    }; \
    AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, ClassName)
