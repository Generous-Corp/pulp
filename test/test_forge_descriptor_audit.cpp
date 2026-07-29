#include <catch2/catch_test_macros.hpp>

#include <pulp/host/forge_catalog_export.hpp>
#include <pulp/host/forge_catalog_index.hpp>
#include <pulp/host/forge_descriptor_audit.hpp>

#include <algorithm>
#include <array>
#include <locale>
#include <set>
#include <string>

using namespace pulp::host;

namespace {

bool has_fault(const std::vector<ForgeAuditFinding>& findings, ForgeAuditFault fault) {
    return std::any_of(findings.begin(), findings.end(),
                       [fault](const auto& f) { return f.fault == fault; });
}

std::string render(const std::vector<ForgeAuditFinding>& findings) {
    std::string out;
    for (const auto& f : findings)
        out += describe_forge_audit_finding(f) + "\n";
    return out;
}

class CommaDecimalFacet : public std::numpunct<char> {
  protected:
    char do_decimal_point() const override {
        return ',';
    }
};

} // namespace

TEST_CASE("every Forge catalog node has an audited semantic descriptor",
          "[forge][catalog][descriptor-audit]") {
    const auto descriptors = forge_catalog_descriptors();
    const auto baked = forge_catalog_baked_params();
    REQUIRE(descriptors.size() == kForgeCatalogNodeCount);
    REQUIRE(baked.size() == descriptors.size());

    const auto findings = audit_forge_catalog(descriptors, baked);
    INFO(render(findings));
    REQUIRE(findings.empty());
}

TEST_CASE("descriptor pack cardinalities prevent representative-only coverage",
          "[forge][catalog][descriptor-audit]") {
    const std::array<std::size_t, 15> actual{{
        forge_lofi::analog_vcf_descriptors().size(),
        character_delay::character_delay_descriptors().size(),
        distortion::distortion_descriptors().size(),
        forge_drum::drum_descriptors().size(),
        dynamics::dynamics_descriptors().size(),
        modulation::effect_modulation_descriptors().size(),
        forge_fdn::fdn_reverb_descriptors().size(),
        forge_lofi::lofi_descriptors().size(),
        forge_modulation::modulation_descriptors().size(),
        pitch::pitch_descriptors().size(),
        saturator::saturator_descriptors().size(),
        sequencing::sequencing_descriptors().size(),
        space::space_descriptors().size(),
        synthesis::synthesis_descriptors().size(),
        tape::tape_descriptors().size(),
    }};
    constexpr std::array<std::size_t, 15> expected{{
        1,
        1,
        1,
        15,
        4,
        9,
        5,
        19,
        5,
        2,
        1,
        6,
        3,
        4,
        1,
    }};
    REQUIRE(actual == expected);
}

TEST_CASE("discrete musical domains retain authored states and bits",
          "[forge][catalog][descriptor-audit]") {
    const auto descriptors = forge_catalog_descriptors();
    const auto harmony = std::find_if(descriptors.begin(), descriptors.end(), [](const auto& node) {
        return node.key == "harmony_engine";
    });
    REQUIRE(harmony != descriptors.end());
    const auto* voice1 = find_param(*harmony, "voice1_interval");
    REQUIRE(voice1 != nullptr);
    REQUIRE(voice1->kind == ForgeParamKind::stepped);
    REQUIRE(voice1->unit == "scale steps");
    REQUIRE(voice1->choices.size() == 29);

    const auto quantizer =
        std::find_if(descriptors.begin(), descriptors.end(),
                     [](const auto& node) { return node.key == "quantize_scale"; });
    REQUIRE(quantizer != descriptors.end());
    const auto* mask = find_param(*quantizer, "scale_mask");
    REQUIRE(mask != nullptr);
    REQUIRE(mask->kind == ForgeParamKind::bitmask);
    REQUIRE(mask->choices.size() == 12);
    REQUIRE(mask->choices.back().value == 2048.0f);
}

TEST_CASE("every semantic descriptor declares unique buildable realizations",
          "[forge][catalog][descriptor-audit]") {
    std::set<std::string_view> type_ids;
    for (const auto& descriptor : forge_catalog_descriptors()) {
        INFO("node " << descriptor.key);
        REQUIRE_FALSE(descriptor.realizations.empty());
        std::set<std::string_view> modes;
        for (const auto& realization : descriptor.realizations) {
            INFO("mode " << realization.mode);
            REQUIRE_FALSE(realization.mode.empty());
            REQUIRE_FALSE(realization.type_id.empty());
            REQUIRE(modes.insert(realization.mode).second);
            REQUIRE(type_ids.insert(realization.type_id).second);
        }
    }
}

TEST_CASE("Forge catalog export joins semantic and baked numeric contracts",
          "[forge][catalog][export]") {
    const auto result = export_forge_catalog_json();
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(result.node_count == kForgeCatalogNodeCount);
    REQUIRE(result.json.find("\"schema_version\":1") != std::string::npos);
    REQUIRE(result.json.find("\"node_count\":78") != std::string::npos);
    REQUIRE(result.json.find("\"key\":\"threshold_db\"") != std::string::npos);
    REQUIRE(result.json.find("\"min\":-60") != std::string::npos);
    REQUIRE(result.json.find("\"max\":0") != std::string::npos);
    REQUIRE(result.json.find("\"default\":-18") != std::string::npos);
    REQUIRE(result.json.find("\"type_id\":\"distortion.to_ground.x1\"") != std::string::npos);
    REQUIRE(result.json.find("\"axis_values\":{\"topology\":\"to_ground\","
                             "\"oversampling\":\"x1\"}") != std::string::npos);
    REQUIRE(result.json.find("\"numeric_contracts\":[{\"realization_mode\":") != std::string::npos);
    REQUIRE(result.json.find("\"realization_mode\":\"dimension_d\",\"min\":0.0500000007,"
                             "\"max\":10,\"default\":0.600000024") != std::string::npos);
}

TEST_CASE("Forge catalog export fails when one indexed node is missing",
          "[forge][catalog][export][negative]") {
    auto descriptors = forge_catalog_descriptors();
    auto baked = forge_catalog_realization_baked_params();
    descriptors.pop_back();
    baked.pop_back();

    const auto result = export_forge_catalog_json(descriptors, baked, kForgeCatalogNodeCount);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.json.empty());
    REQUIRE(result.error.find("node count mismatch") != std::string::npos);
}

TEST_CASE("Forge catalog export rejects a choice outside one realization contract",
          "[forge][catalog][export][negative]") {
    auto descriptors = forge_catalog_descriptors();
    auto baked = forge_catalog_realization_baked_params();
    auto tape = std::find_if(descriptors.begin(), descriptors.end(),
                             [](const auto& node) { return node.key == "tape_machine"; });
    REQUIRE(tape != descriptors.end());
    auto eq = std::find_if(tape->params.begin(), tape->params.end(),
                           [](const auto& param) { return param.key == "eq_curve"; });
    REQUIRE(eq != tape->params.end());
    REQUIRE_FALSE(eq->choices.front().realization_modes.empty());
    eq->choices.front().realization_modes.clear();

    const auto result = export_forge_catalog_json(descriptors, baked, kForgeCatalogNodeCount);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.json.empty());
    REQUIRE(result.error.find("choice is outside its realization contract") != std::string::npos);
}

TEST_CASE("Forge catalog export numbers ignore the process-global locale",
          "[forge][catalog][export]") {
    const auto original = std::locale();
    std::locale::global(std::locale(original, new CommaDecimalFacet));
    const auto result = export_forge_catalog_json();
    std::locale::global(original);

    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(result.json.find("\"default\":1.20000005") != std::string::npos);
    REQUIRE(result.json.find("\"default\":1,20000005") == std::string::npos);
}

TEST_CASE("fuzz descriptors agree with the DSP they annotate", "[forge][catalog]") {
    const auto descriptor = fuzz::descriptor();
    const auto node = fuzz::make_fuzz_node(fuzz::Device::silicon);

    const auto findings = audit_forge_descriptor(descriptor, node.baked_params);
    INFO(render(findings));
    REQUIRE(findings.empty());
}

TEST_CASE("every fuzz realization is a registered type id", "[forge][catalog]") {
    const auto descriptor = fuzz::descriptor();
    REQUIRE(descriptor.realizations.size() == 4);

    // Each declared mode must name the type_id its axis settings actually build,
    // or a graph authored against the descriptor names a node that never
    // registers.
    for (const auto& realization : descriptor.realizations) {
        const bool germanium = realization.mode.find("germanium") != std::string_view::npos;
        const bool oversampled = realization.mode.find("x4") != std::string_view::npos;
        const auto* expected =
            fuzz::type_id(germanium ? fuzz::Device::germanium : fuzz::Device::silicon, oversampled);
        INFO("mode " << realization.mode);
        REQUIRE(realization.type_id == expected);
    }
}

TEST_CASE("descriptor lookup is by stable key", "[forge][catalog]") {
    const auto descriptor = fuzz::descriptor();

    const auto* mix = find_param(descriptor, "mix");
    REQUIRE(mix != nullptr);
    REQUIRE(mix->id == fuzz::kMix);
    REQUIRE(mix->unit == "%");

    REQUIRE(find_param(descriptor, "no_such_param") == nullptr);
}

// ── negative controls ────────────────────────────────────────────────────────
// The audit's whole value is failing closed. Each of these mutates a healthy
// descriptor in exactly one way and requires the matching fault, so a future
// refactor that quietly stops checking one direction fails here.

TEST_CASE("a newly baked parameter with no descriptor is a failure", "[forge][catalog]") {
    const auto descriptor = fuzz::descriptor();
    auto baked = fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params;
    baked.push_back({99, 0.0f, 1.0f, 0.0f}); // DSP grew a control

    const auto findings = audit_forge_descriptor(descriptor, baked);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::undescribed_param));
}

TEST_CASE("a descriptor for a removed parameter is a failure", "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    auto baked = fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params;
    // DSP dropped the control; the descriptor did not.
    baked.erase(std::remove_if(baked.begin(), baked.end(),
                               [](const auto& p) { return p.id == fuzz::kMix; }),
                baked.end());

    const auto findings = audit_forge_descriptor(descriptor, baked);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::stale_descriptor));
}

TEST_CASE("a stepped parameter with no named states is a failure", "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    auto* drift = const_cast<ForgeParamDescriptor*>(find_param(descriptor, "drift_enabled"));
    REQUIRE(drift != nullptr);
    drift->choices.clear();

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::missing_choices));
}

TEST_CASE("a named state outside the baked range is a failure", "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    auto* drift = const_cast<ForgeParamDescriptor*>(find_param(descriptor, "drift_enabled"));
    REQUIRE(drift != nullptr);
    drift->choices = {{"off", "Off", 0.0f}, {"on", "On", 7.0f}}; // baked max is 1

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::choice_out_of_range));
}

TEST_CASE("a logarithmic curve over a zero-inclusive range is a failure", "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    auto* mix = const_cast<ForgeParamDescriptor*>(find_param(descriptor, "mix"));
    REQUIRE(mix != nullptr);
    mix->curve = ForgeParamCurve::logarithmic;

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::invalid_curve));
}

TEST_CASE("restricting a parameter to an undeclared realization is a failure", "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    descriptor.params.front().realization_modes = {"nickel_x2"};

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::unknown_realization));
}

TEST_CASE("two parameters sharing a stable key is a failure", "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    descriptor.params[1].key = descriptor.params[0].key;

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::duplicate_key));
}

TEST_CASE("a blank description is a failure", "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    descriptor.params[0].description = "";

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::empty_field));
}

TEST_CASE("two catalog entries sharing a node key is a failure", "[forge][catalog]") {
    const auto descriptor = fuzz::descriptor();
    const auto baked = fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params;

    const auto findings = audit_forge_catalog({descriptor, descriptor}, {baked, baked});
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::duplicate_key));
}
