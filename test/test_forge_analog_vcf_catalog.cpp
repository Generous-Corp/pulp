#include <catch2/catch_test_macros.hpp>

#include <pulp/host/forge_analog_vcf_catalog.hpp>

#include <array>

namespace lofi = pulp::host::forge_lofi;

TEST_CASE("Forge analog VCF exposes four stable Pulp identities and one param contract",
          "[host][forge][analog-vcf][contract]") {
    using Voicing = pulp::signal::AnalogVcf::Voicing;
    struct Case {
        Voicing voicing;
        const char* type_id;
    };
    constexpr std::array cases{
        Case{Voicing::juno, "vcf.juno"},
        Case{Voicing::jupiter, "vcf.jupiter"},
        Case{Voicing::prophet5, "vcf.prophet5"},
        Case{Voicing::minimoog, "vcf.minimoog"},
    };

    for (const auto& item : cases) {
        const auto node = lofi::make_analog_vcf_node(item.voicing);
        CHECK(node.type_id == item.type_id);
        CHECK(node.lowerable);
        REQUIRE(node.baked_params.size() == 4);
        CHECK(node.baked_params[0].id == lofi::kAnalogVcfCutoff);
        CHECK(node.baked_params[0].min_value == 0.0f);
        CHECK(node.baked_params[0].max_value == 1.0f);
        CHECK(node.baked_params[0].default_value == 0.5f);
        CHECK(node.baked_params[1].id == lofi::kAnalogVcfCutoffMod);
        CHECK(node.baked_params[1].min_value == -5.0f);
        CHECK(node.baked_params[1].max_value == 5.0f);
        CHECK(node.baked_params[2].id == lofi::kAnalogVcfResonance);
        CHECK(node.baked_params[3].id == lofi::kAnalogVcfDriveDb);
        CHECK(node.baked_params[3].min_value == -24.0f);
        CHECK(node.baked_params[3].max_value == 48.0f);
    }
}
