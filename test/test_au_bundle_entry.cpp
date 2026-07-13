// AU v2 multi-plugin-bundle entry-macro test (Apple-only).
//
// Proves the deliverable a single-plugin binary cannot: two plugins hosted in
// ONE binary via the bundle entry macros. It pins three things a portable
// registry test can't:
//   * the PULP_AU_BUNDLE_* macros expand and compile;
//   * `AUSDK_COMPONENT_ENTRY` is legal N-times per binary (distinct generated
//     factory symbols, no ODR collision);
//   * each expansion registers its OWN keyed entry, whose factory builds
//     exactly its own processor — the lexical per-component binding that lets
//     one binary expose many plugins without a shared global.
//
// Instantiating the generated AU classes needs a live AudioComponentInstance
// (a full host), which auval/REAPER cover; here we assert the registry wiring
// the macro produces, which is the part that regresses silently.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/au_v2_entry.hpp>  // PULP_AU_BUNDLE_MIDI_PLUGIN + the AU SDK
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>

#include <memory>
#include <string>

namespace {

template <char Tag>
class BundleTestProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = (Tag == 'A') ? "BundleA" : "BundleB",
            .manufacturer = "PulpTest",
            .bundle_id = (Tag == 'A') ? "com.pulp.test.bundle.a"
                                      : "com.pulp.test.bundle.b",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2}},
            .output_buses = {{"Main Out", 2}},
            .accepts_midi = true,
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
        if (auto* out = audio.main_output()) {
            pulp::audio::BufferView<const float> empty;
            process(*out, empty, mi, mo, ctx);
        }
    }
};

std::unique_ptr<pulp::format::Processor> create_bundle_a() {
    return std::make_unique<BundleTestProcessor<'A'>>();
}
std::unique_ptr<pulp::format::Processor> create_bundle_b() {
    return std::make_unique<BundleTestProcessor<'B'>>();
}

}  // namespace

// Two `aumf` plugins in one binary. This is exactly how a Bitches Brew combined
// bundle wires its modules — expand once per module, each with its own factory
// and AU codes. The registrars run at static init, so the entries exist before
// any test body executes.
//
// BundleAAU deliberately passes a MISMATCHED `.factory` (create_bundle_b) in
// its braced-init to prove the macro ignores it: factory_fn (create_bundle_a)
// is the single source of truth, force-assigned onto the registration so the
// factory the host constructs and the factory the registry reports can never
// desync. BundleBAU omits `.factory` — the normal, documented usage.
PULP_AU_BUNDLE_MIDI_PLUGIN(BundleAAU, create_bundle_a,
    {.id = "com.pulp.test.bundle.a",
     .factory = create_bundle_b,  // wrong on purpose; must be overridden
     .au_type = 'aumf',
     .au_subtype = 'BnA1',
     .au_manufacturer = 'PlpT'})

PULP_AU_BUNDLE_MIDI_PLUGIN(BundleBAU, create_bundle_b,
    {.id = "com.pulp.test.bundle.b",
     .au_type = 'aumf',
     .au_subtype = 'BnB1',
     .au_manufacturer = 'PlpT'})

TEST_CASE("bundle entry macros register two plugins in one binary, each bound "
          "to its own factory",
          "[au][au-v2][bundle][registry]")
{
    // Both static-init registrars ran. Other TUs in this binary register
    // nothing at static init, so the two bundle entries are the only ones.
    auto plugins = pulp::format::registered_plugins();
    REQUIRE(plugins.size() == 2);

    const auto* a = pulp::format::find_plugin("com.pulp.test.bundle.a");
    const auto* b = pulp::format::find_plugin("com.pulp.test.bundle.b");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    // factory_fn is authoritative: BundleAAU's registration passed a mismatched
    // .factory (create_bundle_b), but the macro force-assigns factory_fn, so the
    // registry reports create_bundle_a — the SAME function the AU ctor binds. No
    // split-brain between the constructed processor and the registry entry.
    REQUIRE(a->factory != nullptr);
    REQUIRE(b->factory != nullptr);
    CHECK(a->factory == create_bundle_a);
    CHECK(b->factory == create_bundle_b);
    CHECK(a->factory != b->factory);

    auto pa = a->factory();
    auto pb = b->factory();
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);
    CHECK(std::string(pa->descriptor().name) == "BundleA");
    CHECK(std::string(pb->descriptor().name) == "BundleB");

    // AU codes survived the macro's braced-init.
    CHECK(a->au_subtype == static_cast<std::uint32_t>('BnA1'));
    CHECK(b->au_subtype == static_cast<std::uint32_t>('BnB1'));

    // A bundle never sets the legacy global slot: keyed registration leaves it
    // null by construction, so a stray global read fails loudly.
    CHECK(pulp::format::registered_factory() == nullptr);
}
