// Side-effecting hot-reload logic fixture for the verify-before-load test.
//
// This module has the SAME parameter contract as the transaction test's live
// plugin (one "Gain" param, id 1, range 0..2) and a distinct 2x-gain DSP, so a
// successful reload is observable — but its defining feature is a namespace-scope
// global whose CONSTRUCTOR writes a marker file the instant the image is loaded.
//
// A shared library's static/global constructors run at dlopen/LoadLibrary time,
// BEFORE any symbol is resolved or called. So the marker is a direct, observable
// proxy for "was this image loaded at all?": if the trust gate rejects a
// tampered/ill-signed pack BEFORE the dlopen, this constructor never runs and the
// marker file is never created. The marker path is taken from the
// PULP_RELOAD_CTOR_MARKER environment variable (empty/unset → no-op) so the test
// controls it per-case.

#include <pulp/format/processor.hpp>
#include <pulp/format/reload/reload_abi.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>

using namespace pulp;

namespace {

// Runs at image-load time (static init). Writing the marker here is the whole
// point of the fixture — it is the observable side effect of a dlopen.
struct LoadMarker {
    LoadMarker() {
        if (const char* path = std::getenv("PULP_RELOAD_CTOR_MARKER"); path && *path) {
            std::ofstream(path, std::ios::binary) << "loaded";
        }
    }
};
const LoadMarker g_load_marker;  // constructed when the image is mapped.

class CtorMarkerGain final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {.name = "ReloadGain", .manufacturer = "Pulp Examples",
                .bundle_id = "com.pulp.reload.gain", .version = "0.3.0",
                .category = format::PluginCategory::Effect,
                .input_buses = {{"In", 2}}, .output_buses = {{"Out", 2}}};
    }
    void define_parameters(state::StateStore& s) override {
        s.add_parameter({.id = 1, .name = "Gain", .unit = "",
                         .range = {0.0f, 2.0f, 1.0f, 0.0f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const float g = state().get_value(1) * 2.0f;  // reloaded behavior: 2x gain
        const std::size_t ch = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < ch; ++c) {
            auto o = out.channel(c);
            auto i = in.channel(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n) o[n] = i[n] * g;
        }
    }
};
}  // namespace

PULP_RELOAD_LOGIC(new CtorMarkerGain())
