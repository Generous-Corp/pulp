#include <catch2/catch_test_macros.hpp>

#include <pulp/host/forge_catalog_export.hpp>
#include <pulp/host/forge_catalog_json.hpp>
#include <pulp/host/forge_descriptor_audit.hpp>
#include <pulp/host/forge_fuzz_catalog.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
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

} // namespace

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

TEST_CASE("named states require unique nonempty tokens and labels", "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    auto* drift = const_cast<ForgeParamDescriptor*>(find_param(descriptor, "drift_enabled"));
    REQUIRE(drift != nullptr);
    REQUIRE(drift->choices.size() == 2);
    drift->choices[0].token = "";
    drift->choices[1].label = drift->choices[0].label;

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::empty_field));
    REQUIRE(has_fault(findings, ForgeAuditFault::duplicate_choice));
}

TEST_CASE("every applicable stepped parameter has a named state per realization",
          "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    auto* drift = const_cast<ForgeParamDescriptor*>(find_param(descriptor, "drift_enabled"));
    REQUIRE(drift != nullptr);
    for (auto& choice : drift->choices)
        choice.realization_modes = {"germanium_x1"};

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params, "silicon_x1");
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

TEST_CASE("restricting a parameter to an undeclared realization is a failure", "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    descriptor.params.front().realization_modes = {"nickel_x2"};

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::unknown_realization));
}

TEST_CASE("restricting a named state to an undeclared realization is a failure",
          "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    auto* drift = const_cast<ForgeParamDescriptor*>(find_param(descriptor, "drift_enabled"));
    REQUIRE(drift != nullptr);
    REQUIRE_FALSE(drift->choices.empty());
    drift->choices.front().realization_modes = {"nickel_x2"};

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::unknown_realization));
}

TEST_CASE("a realization missing an axis selection is a failure", "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    REQUIRE_FALSE(descriptor.realizations.front().settings.empty());
    descriptor.realizations.front().settings.pop_back();

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::missing_axis_setting));
}

TEST_CASE("a realization cannot select an undeclared axis", "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    descriptor.realizations.front().settings.push_back({"temperature", "hot"});

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::unexpected_axis_setting));
}

TEST_CASE("a realization cannot select an undeclared axis value", "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    REQUIRE_FALSE(descriptor.realizations.front().settings.empty());
    descriptor.realizations.front().settings.front().value = "vacuum_tube";

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::unknown_axis_value));
}

TEST_CASE("two parameters sharing a stable key is a failure", "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    descriptor.params[1].key = descriptor.params[0].key;

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::duplicate_key));
}

TEST_CASE("a constructed realization cannot bake a parameter id twice", "[forge][catalog]") {
    const auto descriptor = fuzz::descriptor();
    auto baked = fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params;
    REQUIRE_FALSE(baked.empty());
    baked.push_back(baked.front());

    const auto findings = audit_forge_descriptor(descriptor, baked);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::duplicate_id));
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

    const auto findings =
        audit_forge_catalog_membership({descriptor, descriptor}, {"fuzz"});
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::duplicate_key));
}

TEST_CASE("an orphan export node is a failure", "[forge][catalog]") {
    auto descriptor = fuzz::descriptor();
    descriptor.key = "orphan";

    const auto findings = audit_forge_catalog_membership({descriptor}, {"fuzz"});
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::missing_catalog_node));
    REQUIRE(has_fault(findings, ForgeAuditFault::unexpected_catalog_node));
}

TEST_CASE("the export joins semantic descriptors to baked numeric ranges", "[forge][catalog]") {
    const auto nodes = forge_catalog_export_nodes();
    const auto findings = audit_forge_catalog_export(nodes);
    INFO(render(findings));
    REQUIRE(findings.empty());
    REQUIRE(nodes.size() == 77);

    for (const auto& node : nodes) {
        INFO("node " << node.descriptor.key);
        REQUIRE_FALSE(node.descriptor.key.empty());
        REQUIRE_FALSE(node.descriptor.params.empty());
        REQUIRE_FALSE(node.descriptor.realizations.empty());
        REQUIRE(node.realizations.size() == node.descriptor.realizations.size());

        for (const auto& declared : node.descriptor.realizations) {
            INFO("declared realization " << declared.mode);
            REQUIRE_FALSE(declared.mode.empty());
            REQUIRE_FALSE(declared.type_id.empty());
            REQUIRE(declared.settings.size() == node.descriptor.axes.size());
            if (!node.descriptor.axes.empty())
                REQUIRE_FALSE(declared.settings.empty());

            std::set<std::string_view> selected_axes;
            for (const auto& setting : declared.settings) {
                REQUIRE_FALSE(setting.axis.empty());
                REQUIRE_FALSE(setting.value.empty());
                REQUIRE(selected_axes.insert(setting.axis).second);
            }
        }

        for (const auto& built : node.realizations) {
            INFO("constructed realization " << built.mode);
            REQUIRE_FALSE(built.mode.empty());
            REQUIRE_FALSE(built.type_id.empty());
            REQUIRE_FALSE(built.baked_params.empty());
            for (const auto& param : built.baked_params) {
                REQUIRE(param.min_value <= param.default_value);
                REQUIRE(param.default_value <= param.max_value);
            }
        }
    }

    const auto fuzz_node =
        std::find_if(nodes.begin(), nodes.end(),
                     [](const auto& node) { return node.descriptor.key == "fuzz"; });
    REQUIRE(fuzz_node != nodes.end());
    const auto* mix = find_param(fuzz_node->descriptor, "mix");
    REQUIRE(mix != nullptr);
    REQUIRE(fuzz_node->realizations.size() == 4);
    const auto& first_realization = fuzz_node->realizations.front();
    const auto baked =
        std::find_if(first_realization.baked_params.begin(), first_realization.baked_params.end(),
                     [mix](const auto& value) { return value.id == mix->id; });
    REQUIRE(baked != first_realization.baked_params.end());
    REQUIRE(baked->min_value == 0.0f);
    REQUIRE(baked->max_value == 1.0f);
    REQUIRE(baked->default_value == 1.0f);
}

TEST_CASE("every declared export realization is constructed and type checked", "[forge][catalog]") {
    auto nodes = forge_catalog_export_nodes();
    REQUIRE_FALSE(nodes.empty());
    nodes.front().realizations.front().type_id = "wrong.type";

    const auto findings = audit_forge_catalog_export(nodes);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::mismatched_type_id));
}

TEST_CASE("a declared export realization cannot be omitted", "[forge][catalog]") {
    auto nodes = forge_catalog_export_nodes();
    REQUIRE_FALSE(nodes.empty());
    nodes.front().realizations.pop_back();

    const auto findings = audit_forge_catalog_export(nodes);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::missing_realization));
}

TEST_CASE("a catalog node missing from the export fails loudly", "[forge][catalog]") {
    auto nodes = forge_catalog_export_nodes();
    REQUIRE_FALSE(nodes.empty());
    nodes.clear();

    const auto findings = audit_forge_catalog_export(nodes);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::missing_catalog_node));
}

TEST_CASE("the committed installed-artifact snapshot matches the audited serializer",
          "[forge][catalog][snapshot]") {
    const auto nodes = forge_catalog_export_nodes();
    const auto findings = audit_forge_catalog_export(nodes);
    INFO(render(findings));
    REQUIRE(findings.empty());

    const std::string serialized = serialize_forge_catalog_json(nodes);
    std::ifstream input(std::string(PULP_SOURCE_DIR) +
                            "/docs/status/forge-catalog.json",
                        std::ios::binary);
    REQUIRE(input);
    const std::string snapshot{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
    REQUIRE(snapshot == serialized);

    // Pin the consumer-facing joins that are easy to accidentally flatten.
    REQUIRE(serialized.find("\"schema\": \"pulp.forge-catalog.v1\"") !=
            std::string::npos);
    REQUIRE(serialized.find("\"settings\": {") != std::string::npos);
    REQUIRE(serialized.find("\"contracts\": [") != std::string::npos);
    REQUIRE(serialized.find("\"realization_modes\": [\"studer\"]") !=
            std::string::npos);
    REQUIRE(serialized.find("\"realization_modes\": [\"cassette\"]") !=
            std::string::npos);
}
