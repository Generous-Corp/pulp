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
extern rack::plugin::Model* modelCARTOG;
extern rack::plugin::Model* modelATT;
extern rack::plugin::Model* modelSEQ;
extern rack::plugin::Model* modelMIX;
extern rack::plugin::Model* modelDUALATN;
extern rack::plugin::Model* modelATTEN;
extern rack::plugin::Model* modelMORPHLFO;
extern rack::plugin::Model* modelFOURPOLE;
extern rack::plugin::Model* modelSTEPS;
extern rack::plugin::Model* modelKICK;
extern rack::plugin::Model* modelSANDH;
extern rack::plugin::Model* modelFOLD;
extern rack::plugin::Model* modelDUALAD;
extern rack::plugin::Model* modelTANGLE;
extern rack::plugin::Model* modelSIXMIX;
extern rack::plugin::Model* modelOFFSETLY;
extern rack::plugin::Model* modelDIVIDELY;
