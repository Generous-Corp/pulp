#include "webview_plugin.hpp"

#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID PulpWebViewPluginUID(
    0x50554C50, 0x57564250, 0x00000651, 0x00000001);

PULP_VST3_PLUGIN(PulpWebViewPluginUID,
                 "PulpWebViewPlugin",
                 Steinberg::Vst::PlugType::kFx,
                 "Pulp",
                 "1.0.0",
                 "https://github.com/Generous-Corp/pulp",
                 pulp::examples::create_pulp_webview_plugin)
