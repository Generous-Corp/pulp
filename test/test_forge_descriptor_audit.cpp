#include <catch2/catch_test_macros.hpp>

#include <pulp/host/forge_descriptor_audit.hpp>
#include <pulp/host/forge_fuzz_catalog.hpp>

#include <algorithm>
#include <string>

using namespace pulp::host;

namespace {

bool has_fault(const std::vector<ForgeAuditFinding>& findings, ForgeAuditFault fault) {
    return std::any_of(findings.begin(), findings.end(),
                       [fault](const auto& f) { return f.fault == fault; });
}

std::string render(const std::vector<ForgeAuditFinding>& findings) {
    std::string out;
    for (const auto& f : findings) out += describe_forge_audit_finding(f) + "\n";
    return out;
}

}  // namespace

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
        const auto* expected = fuzz::type_id(
            germanium ? fuzz::Device::germanium : fuzz::Device::silicon, oversampled);
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

TEST_CASE("a newly baked parameter with no descriptor is a failure",
          "[forge][catalog]") {
    const auto descriptor = fuzz::descriptor();
    auto baked = fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params;
    baked.push_back({99, 0.0f, 1.0f, 0.0f});  // DSP grew a control

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

TEST_CASE("a stepped parameter with no named states is a failure",
          "[forge][catalog]") {
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
    drift->choices = {{"off", "Off", 0.0f}, {"on", "On", 7.0f}};  // baked max is 1

    const auto findings = audit_forge_descriptor(
        descriptor, fuzz::make_fuzz_node(fuzz::Device::silicon).baked_params);
    INFO(render(findings));
    REQUIRE(has_fault(findings, ForgeAuditFault::choice_out_of_range));
}

TEST_CASE("restricting a parameter to an undeclared realization is a failure",
          "[forge][catalog]") {
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
