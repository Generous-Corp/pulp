// Plugin registry tests — the multi-plugin-bundle registration core.
//
// The registry connects format adapters to processor factories. Historically
// one binary hosted one plugin via a single global slot (`registered_factory`).
// A multi-plugin bundle (one binary exposing many plugins, Silent Way style)
// instead registers KEYED entries (`PluginRegistration`, keyed by reverse-DNS
// id) and binds each component to its own factory lexically, so no global
// lookup happens on any instantiation path.
//
// These tests pin the contract both modes depend on:
//   * the legacy global slot is last-write-wins and untouched by keyed
//     registration (so the adapter save/restore idiom keeps working, and a
//     pure bundle leaves the global null by construction);
//   * keyed registration appends discoverable entries with their factory + AU
//     codes + per-plugin editor assets intact, and each entry's factory builds
//     exactly its own processor;
//   * one plugin registered under several AU types (aumf + aumu + augn) shares
//     one factory across entries;
//   * lookups by id and enumeration behave.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>

#include <cstdint>
#include <memory>
#include <string>

using pulp::format::find_plugin;
using pulp::format::PluginRegistration;
using pulp::format::register_plugin;
using pulp::format::registered_factory;
using pulp::format::registered_plugins;
using pulp::format::reset_registry_for_testing;

namespace {

// Two minimal processors distinguishable only by their descriptor name, so a
// test can assert which factory built the instance it got back.
template <char Tag>
class TaggedProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = (Tag == 'A') ? "RegistryTestA" : "RegistryTestB",
            .manufacturer = "PulpTest",
            .bundle_id = (Tag == 'A') ? "com.pulp.test.registry.a"
                                      : "com.pulp.test.registry.b",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Main In", 2}},
            .output_buses = {{"Main Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        for (std::size_t c = 0; c < output.num_channels(); ++c) {
            float* dst = output.channel_ptr(c);
            for (std::size_t n = 0; n < output.num_samples(); ++n) dst[n] = 0.0f;
        }
    }

    void process(pulp::format::ProcessBuffers& audio,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& context) override {
        if (auto* out = audio.main_output()) {
            pulp::audio::BufferView<const float> empty_input;
            process(*out, empty_input, midi_in, midi_out, context);
        }
    }
};

std::unique_ptr<pulp::format::Processor> create_a() {
    return std::make_unique<TaggedProcessor<'A'>>();
}
std::unique_ptr<pulp::format::Processor> create_b() {
    return std::make_unique<TaggedProcessor<'B'>>();
}

std::string built_name(pulp::format::ProcessorFactory factory) {
    REQUIRE(factory != nullptr);
    auto p = factory();
    REQUIRE(p != nullptr);
    return std::string(p->descriptor().name);
}

// FourCC helper mirroring the AU 4-char codes stored on a registration.
constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return (std::uint32_t(std::uint8_t(a)) << 24) |
           (std::uint32_t(std::uint8_t(b)) << 16) |
           (std::uint32_t(std::uint8_t(c)) << 8) | std::uint32_t(std::uint8_t(d));
}

}  // namespace

TEST_CASE("keyed registration is discoverable, distinct, and factory-correct",
          "[format][registry][bundle]")
{
    reset_registry_for_testing();

    register_plugin(PluginRegistration{
        .id = "com.pulp.test.registry.a",
        .factory = create_a,
        .au_type = fourcc('a', 'u', 'm', 'f'),
        .au_subtype = fourcc('R', 'g', 'A', '1'),
        .au_manufacturer = fourcc('P', 'l', 'p', 'T'),
        .ui_script = "a.js",
        .ui_theme = "a-theme",
        .ui_asset_roots = "a/assets",
    });
    register_plugin(PluginRegistration{
        .id = "com.pulp.test.registry.b",
        .factory = create_b,
        .au_type = fourcc('a', 'u', 'm', 'f'),
        .au_subtype = fourcc('R', 'g', 'B', '1'),
        .au_manufacturer = fourcc('P', 'l', 'p', 'T'),
    });

    auto all = registered_plugins();
    REQUIRE(all.size() == 2);

    const PluginRegistration* a = find_plugin("com.pulp.test.registry.a");
    const PluginRegistration* b = find_plugin("com.pulp.test.registry.b");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    // Each entry's factory builds exactly its own processor — the whole point of
    // lexical per-component binding.
    CHECK(built_name(a->factory) == "RegistryTestA");
    CHECK(built_name(b->factory) == "RegistryTestB");

    // AU codes and per-plugin editor assets survive registration intact.
    CHECK(a->au_subtype == fourcc('R', 'g', 'A', '1'));
    CHECK(std::string(a->ui_script) == "a.js");
    CHECK(std::string(a->ui_theme) == "a-theme");
    CHECK(std::string(a->ui_asset_roots) == "a/assets");

    // Entries with no editor assets fall back to null (compile-time defines).
    CHECK(b->ui_script == nullptr);

    // Keyed registration never touches the legacy global slot.
    CHECK(registered_factory() == nullptr);
}

TEST_CASE("one plugin under multiple AU types shares one factory",
          "[format][registry][bundle]")
{
    reset_registry_for_testing();

    // Silent Way exposes a module as several AU types at once (aumf + aumu +
    // augn). Model that as three entries sharing one id + factory, differing
    // only in au_type.
    for (std::uint32_t type : {fourcc('a', 'u', 'm', 'f'),
                               fourcc('a', 'u', 'm', 'u'),
                               fourcc('a', 'u', 'g', 'n')}) {
        register_plugin(PluginRegistration{
            .id = "com.pulp.test.registry.multi",
            .factory = create_a,
            .au_type = type,
            .au_subtype = fourcc('M', 'u', 'l', 't'),
        });
    }

    REQUIRE(registered_plugins().size() == 3);

    // Lookup by id resolves to the first matching entry; all three carry the
    // same factory.
    const PluginRegistration* first = find_plugin("com.pulp.test.registry.multi");
    REQUIRE(first != nullptr);
    CHECK(first->au_type == fourcc('a', 'u', 'm', 'f'));

    int count = 0;
    for (const auto& reg : registered_plugins()) {
        CHECK(reg.factory == create_a);
        ++count;
    }
    CHECK(count == 3);
}

TEST_CASE("legacy global slot is last-write-wins and independent of keyed table",
          "[format][registry]")
{
    reset_registry_for_testing();

    // Fresh state: no global, no keyed entries.
    CHECK(registered_factory() == nullptr);
    CHECK(registered_plugins().empty());

    // Last write wins — the save/restore + swap idiom adapter tests rely on.
    register_plugin(create_a);
    CHECK(registered_factory() == create_a);
    register_plugin(create_b);
    CHECK(registered_factory() == create_b);
    register_plugin(create_a);
    CHECK(registered_factory() == create_a);

    // The legacy path does NOT populate the keyed enumeration.
    CHECK(registered_plugins().empty());

    // Restoring to null is legal and does not wedge the slot.
    register_plugin(nullptr);
    CHECK(registered_factory() == nullptr);
}

TEST_CASE("lookup rejects null and unknown ids", "[format][registry]")
{
    reset_registry_for_testing();
    register_plugin(PluginRegistration{
        .id = "com.pulp.test.registry.only",
        .factory = create_a,
    });

    CHECK(find_plugin(nullptr) == nullptr);
    CHECK(find_plugin("com.pulp.test.registry.missing") == nullptr);
    CHECK(find_plugin("com.pulp.test.registry.only") != nullptr);
}
