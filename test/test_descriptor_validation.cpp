// validate_descriptor() tests (workstream 01 slice 1.9).

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/descriptor_validation.hpp>

using namespace pulp::format;

namespace {

PluginDescriptor well_formed_effect() {
    PluginDescriptor d;
    d.name = "Pulp Reverb";
    d.manufacturer = "PulpCo";
    d.bundle_id = "com.pulpco.reverb";
    d.version = "1.0.0";
    d.category = PluginCategory::Effect;
    return d;
}

bool has_error_on(const std::vector<DescriptorIssue>& issues,
                  const std::string& field) {
    for (const auto& i : issues) {
        if (i.severity == DescriptorIssueSeverity::Error && i.field == field)
            return true;
    }
    return false;
}

bool has_warning_on(const std::vector<DescriptorIssue>& issues,
                    const std::string& field) {
    for (const auto& i : issues) {
        if (i.severity == DescriptorIssueSeverity::Warning && i.field == field)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("well-formed effect passes with no issues",
          "[format][descriptor-validation]") {
    auto issues = validate_descriptor(well_formed_effect());
    REQUIRE(issues.empty());
    REQUIRE(descriptor_is_valid(issues));
}

TEST_CASE("empty name/manufacturer/bundle_id are errors",
          "[format][descriptor-validation]") {
    PluginDescriptor d;
    auto issues = validate_descriptor(d);
    REQUIRE(has_error_on(issues, "name"));
    REQUIRE(has_error_on(issues, "manufacturer"));
    REQUIRE(has_error_on(issues, "bundle_id"));
    REQUIRE_FALSE(descriptor_is_valid(issues));
}

TEST_CASE("non-reverse-DNS bundle_id produces a warning only",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.bundle_id = "MyPlugin";   // no dots
    auto issues = validate_descriptor(d);
    REQUIRE(has_warning_on(issues, "bundle_id"));
    REQUIRE(descriptor_is_valid(issues));   // warnings don't fail the check
}

TEST_CASE("missing output bus is an error",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.output_buses.clear();
    auto issues = validate_descriptor(d);
    REQUIRE(has_error_on(issues, "output_buses"));
}

TEST_CASE("zero-channel main output is an error",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.output_buses = {{"Silent", 0, false}};
    auto issues = validate_descriptor(d);
    REQUIRE(has_error_on(issues, "output_buses"));
}

TEST_CASE("instrument with non-optional stereo input warns",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.category = PluginCategory::Instrument;
    // d still has default {{"Main In", 2, false}}
    auto issues = validate_descriptor(d);
    REQUIRE(has_warning_on(issues, "input_buses"));
    REQUIRE(descriptor_is_valid(issues));
}

TEST_CASE("MidiEffect without accepts_midi warns",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.category = PluginCategory::MidiEffect;
    d.accepts_midi = false;
    auto issues = validate_descriptor(d);
    REQUIRE(has_warning_on(issues, "accepts_midi"));
}

TEST_CASE("supports_mpe without accepts_midi warns",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.supports_mpe = true;
    d.accepts_midi = false;
    auto issues = validate_descriptor(d);
    REQUIRE(has_warning_on(issues, "accepts_midi"));
}

TEST_CASE("MidiEffect without audio output is valid",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.category = PluginCategory::MidiEffect;
    d.accepts_midi = true;
    d.produces_midi = true;
    d.output_buses.clear();
    auto issues = validate_descriptor(d);
    for (const auto& i : issues) {
        if (i.severity == DescriptorIssueSeverity::Error) {
            REQUIRE(i.field != "output_buses");
        }
    }
    REQUIRE(descriptor_is_valid(issues));
}

TEST_CASE("reverse-DNS bundle_id passes",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.bundle_id = "com.example.sub.plugin";
    auto issues = validate_descriptor(d);
    for (const auto& i : issues) {
        REQUIRE(i.field != "bundle_id");
    }
}
