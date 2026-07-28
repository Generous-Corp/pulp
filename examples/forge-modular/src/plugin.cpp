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
    p->addModel(modelVCO);
}
