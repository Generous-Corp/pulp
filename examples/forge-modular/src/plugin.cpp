#include "plugin.hpp"

rack::plugin::Plugin* pluginInstance = nullptr;

// Rack declares init() with no visibility attribute, so a plugin compiled with
// -fvisibility=hidden exports NOTHING and dlsym("init") returns null — the
// plugin simply fails to load, with no diagnostic. The explicit default
// visibility here is what makes hidden-by-default safe, and it leaves the
// binary exporting exactly one symbol, which is the minimal surface for
// sharing a process with another Pulp-based binary.
extern "C" __attribute__((visibility("default")))
void init(rack::plugin::Plugin* p) {
    pluginInstance = p;
    // Signal order, which is also how they appear in the Module Browser.
    p->addModel(modelVCO);
    p->addModel(modelVCF);
    p->addModel(modelVCA);
    p->addModel(modelENV);
    p->addModel(modelLFO);
    p->addModel(modelEUCLID);
    p->addModel(modelSEQ);
    p->addModel(modelMIX);
    p->addModel(modelCARTOG);
    p->addModel(modelATT);
    p->addModel(modelMULT);
    p->addModel(modelDUALATN);
    p->addModel(modelATTEN);
    p->addModel(modelMORPHLFO);
    p->addModel(modelFOURPOLE);
    p->addModel(modelSTEPS);
    p->addModel(modelKICK);
    p->addModel(modelSANDH);
    p->addModel(modelFOLD);
    p->addModel(modelDUALAD);
    p->addModel(modelTANGLE);
    p->addModel(modelSIXMIX);
    p->addModel(modelOFFSETLY);
    p->addModel(modelDIVIDELY);
}
