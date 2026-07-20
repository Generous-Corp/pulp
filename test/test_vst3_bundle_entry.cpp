// VST3 multi-plugin-bundle entry-macro test.
//
// Proves the deliverable a single-plugin binary cannot: two plugins exposed
// from ONE VST3 factory via the bundle macros. It pins:
//   * PULP_VST3_BUNDLE_PLUGIN / _FACTORY_BEGIN / _BUNDLE_CLASS / _FACTORY_END
//     expand and compile, and the generated GetPluginFactory() lists BOTH
//     classes (countClasses() == 2) with the right names/IDs;
//   * each class's create function binds its OWN ProcessorFactory (the Ident
//     token links _PLUGIN to _CLASS at compile time);
//   * each keyed registry entry carries the correct factory — force-assigned
//     from factory_fn, so a mismatched `.factory` in the braced-init is ignored.
//
// countClasses()/getClassInfo() read the class registrations without
// instantiating a processor, so no live VST3 host is needed here.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/format/vst3_entry.hpp>

#include <cstring>
#include <memory>
#include <string>

namespace {

template <char Tag>
class Vst3BundleProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = (Tag == 'A') ? "Vst3BundleA" : "Vst3BundleB",
            .manufacturer = "PulpTest",
            .bundle_id = (Tag == 'A') ? "com.pulp.test.vst3bundle.a"
                                      : "com.pulp.test.vst3bundle.b",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2}},
            .output_buses = {{"Main Out", 2}},
        };
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        for (std::size_t c = 0; c < out.num_channels(); ++c) {
            float* dst = out.channel_ptr(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n) dst[n] = 0.0f;
        }
    }
    void process(pulp::format::ProcessBuffers& audio,
                 pulp::midi::MidiBuffer& mi, pulp::midi::MidiBuffer& mo,
                 const pulp::format::ProcessContext& ctx) override {
        if (auto* o = audio.main_output()) {
            pulp::audio::BufferView<const float> empty;
            process(*o, empty, mi, mo, ctx);
        }
    }
};

std::unique_ptr<pulp::format::Processor> create_vst3_a() {
    return std::make_unique<Vst3BundleProcessor<'A'>>();
}
std::unique_ptr<pulp::format::Processor> create_vst3_b() {
    return std::make_unique<Vst3BundleProcessor<'B'>>();
}

const Steinberg::FUID kVst3BundleAUID(0x50554C50, 0x42554E41, 0x00000001, 0x00000001);
const Steinberg::FUID kVst3BundleBUID(0x50554C50, 0x42554E42, 0x00000001, 0x00000001);

}  // namespace

// PluginA deliberately passes a mismatched `.factory` to prove factory_fn wins.
PULP_VST3_BUNDLE_PLUGIN(BundleA, create_vst3_a,
    {.id = "com.pulp.test.vst3bundle.a", .factory = create_vst3_b})
PULP_VST3_BUNDLE_PLUGIN(BundleB, create_vst3_b,
    {.id = "com.pulp.test.vst3bundle.b"})

PULP_VST3_FACTORY_BEGIN("PulpTest", "https://github.com/Generous-Corp/pulp",
                        "mailto:info@example.com")
    PULP_VST3_BUNDLE_CLASS(BundleA, kVst3BundleAUID, "Vst3BundleA",
                           Steinberg::Vst::PlugType::kFx, "1.0.0")
    PULP_VST3_BUNDLE_CLASS(BundleB, kVst3BundleBUID, "Vst3BundleB",
                           Steinberg::Vst::PlugType::kFx, "1.0.0")
PULP_VST3_FACTORY_END

TEST_CASE("VST3 bundle factory exposes two classes from one binary",
          "[vst3][bundle][registry]")
{
    Steinberg::IPluginFactory* factory = GetPluginFactory();
    REQUIRE(factory != nullptr);

    // The generated factory lists BOTH plugins as distinct classes.
    REQUIRE(factory->countClasses() == 2);

    bool saw_a = false, saw_b = false;
    for (Steinberg::int32 i = 0; i < factory->countClasses(); ++i) {
        Steinberg::PClassInfo info{};
        REQUIRE(factory->getClassInfo(i, &info) == Steinberg::kResultOk);
        if (std::strcmp(info.name, "Vst3BundleA") == 0) saw_a = true;
        if (std::strcmp(info.name, "Vst3BundleB") == 0) saw_b = true;
    }
    CHECK(saw_a);
    CHECK(saw_b);
    factory->release();
}

TEST_CASE("VST3 bundle registers keyed entries; factory_fn is authoritative",
          "[vst3][bundle][registry]")
{
    const auto* a = pulp::format::find_plugin("com.pulp.test.vst3bundle.a");
    const auto* b = pulp::format::find_plugin("com.pulp.test.vst3bundle.b");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    // BundleA passed a mismatched .factory (create_vst3_b); the macro overrides
    // it with factory_fn (create_vst3_a), so the registry and the class's create
    // function agree — no split-brain.
    CHECK(a->factory == create_vst3_a);
    CHECK(b->factory == create_vst3_b);

    auto pa = a->factory();
    auto pb = b->factory();
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);
    CHECK(std::string(pa->descriptor().name) == "Vst3BundleA");
    CHECK(std::string(pb->descriptor().name) == "Vst3BundleB");
}
