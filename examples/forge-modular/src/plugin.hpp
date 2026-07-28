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
extern rack::plugin::Model* modelENV;
extern rack::plugin::Model* modelVCF;
extern rack::plugin::Model* modelVCA;
extern rack::plugin::Model* modelEUCLID;
extern rack::plugin::Model* modelLFO;
extern rack::plugin::Model* modelMULT;
extern rack::plugin::Model* modelATT;
extern rack::plugin::Model* modelSEQ;
