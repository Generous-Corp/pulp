#pragma once

// Forge Modular — a Eurorack module set for VCV Rack, built on Pulp.
//
// The panel art, the parameter/port configuration and the widget placement in
// generated_modules.hpp all derive from one layout manifest, so a control and
// its label cannot disagree about where they are.

#include <rack.hpp>

#include "generated_modules.hpp"

extern rack::plugin::Plugin* pluginInstance;

extern rack::plugin::Model* modelVCO;
