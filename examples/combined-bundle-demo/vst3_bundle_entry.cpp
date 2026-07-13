// Combined-bundle demo — VST3 entry. Two plugins, one GetPluginFactory().
// One PULP_VST3_BUNDLE_PLUGIN per plugin (file scope), then one factory block
// listing a PULP_VST3_BUNDLE_CLASS per plugin. The `Ident` token links each
// pair at compile time; the braced `.id` feeds the shared keyed registry for
// cross-format lookup.

#include "bundle_plugins.hpp"
#include <pulp/format/vst3_entry.hpp>

// Stable, unique per class — never change across versions.
static const Steinberg::FUID kBundleGainUID(0x50554C50, 0x42444700, 0x00000001, 0x00000001);
static const Steinberg::FUID kBundleWidthUID(0x50554C50, 0x42445700, 0x00000001, 0x00000002);

PULP_VST3_BUNDLE_PLUGIN(BundleGain, pulp::examples::bundle::create_gain,
    {.id = "com.pulp.bundle-demo.gain"})
PULP_VST3_BUNDLE_PLUGIN(BundleWidth, pulp::examples::bundle::create_width,
    {.id = "com.pulp.bundle-demo.width"})

PULP_VST3_FACTORY_BEGIN("Pulp", "https://github.com/danielraffel/pulp",
                        "mailto:support@pulp.audio")
    PULP_VST3_BUNDLE_CLASS(BundleGain, kBundleGainUID, "Bundle Gain",
                           Steinberg::Vst::PlugType::kFx, "1.0.0")
    PULP_VST3_BUNDLE_CLASS(BundleWidth, kBundleWidthUID, "Bundle Width",
                           Steinberg::Vst::PlugType::kFx, "1.0.0")
PULP_VST3_FACTORY_END
