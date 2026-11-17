#include "allpass_processor.hpp"

#include <emscripten/emscripten.h>

#include <string>

std::unique_ptr<pulp::format::Processor> pulp_wam_make_processor() {
    return pulp::examples::create_sample_region_allpass();
}

namespace {

bool well_formed(const std::string& id) {
    return id.size() == 36 && id.rfind("nid-", 0) == 0;
}

} // namespace

// Browser acceptance probe, called from the AudioWorklet global scope the DSP
// runs in: two graphs each add nodes, and every minted authored ID must be
// well formed and distinct. Returns the ID length (36) on success and a
// negative code naming the first failed expectation otherwise.
extern "C" EMSCRIPTEN_KEEPALIVE int sample_region_wam_authored_id_probe() {
    pulp::host::SignalGraph first;
    pulp::host::SignalGraph second;
    const auto a = first.add_gain_node("A");
    const auto b = first.add_gain_node("B");
    const auto c = second.add_gain_node("A");
    if (a == 0 || b == 0 || c == 0)
        return -1;
    const auto& id_a = first.node(a)->authored_id.str();
    const auto& id_b = first.node(b)->authored_id.str();
    const auto& id_c = second.node(c)->authored_id.str();
    if (!well_formed(id_a) || !well_formed(id_b) || !well_formed(id_c))
        return -2;
    if (id_a == id_b)
        return -3;
    if (id_a == id_c)
        return -4;
    return static_cast<int>(id_a.size());
}
