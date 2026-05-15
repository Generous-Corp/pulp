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

std::size_t warning_count_on(const std::vector<DescriptorIssue>& issues,
                             const std::string& field) {
    std::size_t count = 0;
    for (const auto& i : issues) {
        if (i.severity == DescriptorIssueSeverity::Warning && i.field == field)
            ++count;
    }
    return count;
}

} // namespace

TEST_CASE("DescriptorValidation: well-formed effect passes with no issues",
          "[format][descriptor-validation]") {
    auto issues = validate_descriptor(well_formed_effect());
    REQUIRE(issues.empty());
    REQUIRE(descriptor_is_valid(issues));
}

TEST_CASE("DescriptorValidation: empty name/manufacturer/bundle_id/version are errors",
          "[format][descriptor-validation]") {
    PluginDescriptor d;
    auto issues = validate_descriptor(d);
    REQUIRE(has_error_on(issues, "name"));
    REQUIRE(has_error_on(issues, "manufacturer"));
    REQUIRE(has_error_on(issues, "bundle_id"));
    REQUIRE(has_error_on(issues, "version"));
    REQUIRE_FALSE(descriptor_is_valid(issues));
}

TEST_CASE("DescriptorValidation: empty version is an error",
          "[format][descriptor-validation][coverage][issue-493]") {
    auto d = well_formed_effect();
    d.version.clear();

    auto issues = validate_descriptor(d);
    REQUIRE(has_error_on(issues, "version"));
    REQUIRE_FALSE(descriptor_is_valid(issues));
}

TEST_CASE("DescriptorValidation: non-reverse-DNS bundle_id produces a warning only",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.bundle_id = "MyPlugin";   // no dots
    auto issues = validate_descriptor(d);
    REQUIRE(has_warning_on(issues, "bundle_id"));
    REQUIRE(descriptor_is_valid(issues));   // warnings don't fail the check
}

TEST_CASE("DescriptorValidation: malformed reverse-DNS bundle_id segments warn only",
          "[format][descriptor-validation][coverage][issue-493]") {
    for (const auto* bundle_id : {
             ".example.plugin",
             "com..plugin",
             "com.example.",
             "com. .plugin",
         }) {
        auto d = well_formed_effect();
        d.bundle_id = bundle_id;

        CAPTURE(bundle_id);
        auto issues = validate_descriptor(d);
        REQUIRE(has_warning_on(issues, "bundle_id"));
        REQUIRE(descriptor_is_valid(issues));
    }
}

TEST_CASE("DescriptorValidation: missing output bus is an error",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.output_buses.clear();
    auto issues = validate_descriptor(d);
    REQUIRE(has_error_on(issues, "output_buses"));
}

TEST_CASE("DescriptorValidation: zero-channel main output is an error",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.output_buses = {{"Silent", 0, false}};
    auto issues = validate_descriptor(d);
    REQUIRE(has_error_on(issues, "output_buses"));
}

TEST_CASE("DescriptorValidation: negative bus channel counts are errors",
          "[format][descriptor-validation][coverage][issue-493]") {
    SECTION("negative main output") {
        auto d = well_formed_effect();
        d.output_buses = {{"Invalid Out", -1, false}};

        auto issues = validate_descriptor(d);
        REQUIRE(has_error_on(issues, "output_buses"));
        REQUIRE_FALSE(descriptor_is_valid(issues));
    }

    SECTION("negative main input") {
        auto d = well_formed_effect();
        d.input_buses = {{"Invalid In", -2, false}};

        auto issues = validate_descriptor(d);
        REQUIRE(has_error_on(issues, "input_buses"));
        REQUIRE_FALSE(descriptor_is_valid(issues));
    }
}

TEST_CASE("DescriptorValidation: negative main input is an error",
          "[format][descriptor-validation][coverage][issue-493]") {
    auto d = well_formed_effect();
    d.input_buses = {{"Invalid In", -2, false}};

    auto issues = validate_descriptor(d);
    REQUIRE(has_error_on(issues, "input_buses"));
    REQUIRE_FALSE(descriptor_is_valid(issues));
}

TEST_CASE("DescriptorValidation: instrument with non-optional stereo input warns",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.category = PluginCategory::Instrument;
    // d still has default {{"Main In", 2, false}}
    auto issues = validate_descriptor(d);
    REQUIRE(has_warning_on(issues, "input_buses"));
    REQUIRE(descriptor_is_valid(issues));
}

TEST_CASE("DescriptorValidation: instrument optional or zero-channel inputs do not warn",
          "[format][descriptor-validation][coverage][issue-493]") {
    SECTION("optional stereo input") {
        auto d = well_formed_effect();
        d.category = PluginCategory::Instrument;
        d.input_buses = {{"Optional In", 2, true}};

        auto issues = validate_descriptor(d);
        REQUIRE_FALSE(has_warning_on(issues, "input_buses"));
        REQUIRE(descriptor_is_valid(issues));
    }

    SECTION("zero-channel main input") {
        auto d = well_formed_effect();
        d.category = PluginCategory::Instrument;
        d.input_buses = {{"No Audio In", 0, false}};

        auto issues = validate_descriptor(d);
        REQUIRE_FALSE(has_warning_on(issues, "input_buses"));
        REQUIRE(descriptor_is_valid(issues));
    }

    SECTION("no input bus") {
        auto d = well_formed_effect();
        d.category = PluginCategory::Instrument;
        d.input_buses.clear();

        auto issues = validate_descriptor(d);
        REQUIRE_FALSE(has_warning_on(issues, "input_buses"));
        REQUIRE(descriptor_is_valid(issues));
    }
}

TEST_CASE("DescriptorValidation: MidiEffect without accepts_midi warns",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.category = PluginCategory::MidiEffect;
    d.accepts_midi = false;
    auto issues = validate_descriptor(d);
    REQUIRE(has_warning_on(issues, "accepts_midi"));
}

TEST_CASE("DescriptorValidation: supports_mpe without accepts_midi warns",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.supports_mpe = true;
    d.accepts_midi = false;
    auto issues = validate_descriptor(d);
    REQUIRE(has_warning_on(issues, "accepts_midi"));
}

TEST_CASE("DescriptorValidation: supports_ump follows the accepts_midi sidecar warning contract",
          "[format][descriptor-validation][coverage][issue-493]") {
    SECTION("UMP without MIDI input warns") {
        auto d = well_formed_effect();
        d.supports_ump = true;
        d.accepts_midi = false;

        auto issues = validate_descriptor(d);
        REQUIRE(has_warning_on(issues, "accepts_midi"));
        REQUIRE(descriptor_is_valid(issues));
    }

    SECTION("accepts_midi suppresses the sidecar warning") {
        auto d = well_formed_effect();
        d.supports_ump = true;
        d.accepts_midi = true;

        auto issues = validate_descriptor(d);
        REQUIRE_FALSE(has_warning_on(issues, "accepts_midi"));
        REQUIRE(descriptor_is_valid(issues));
    }
}

TEST_CASE("DescriptorValidation: MIDI capability warnings accumulate without invalidating descriptor",
          "[format][descriptor-validation][coverage][issue-646]") {
    auto d = well_formed_effect();
    d.category = PluginCategory::MidiEffect;
    d.accepts_midi = false;
    d.supports_mpe = true;
    d.supports_ump = true;

    auto issues = validate_descriptor(d);
    REQUIRE(warning_count_on(issues, "accepts_midi") == 2);
    REQUIRE(descriptor_is_valid(issues));
}

TEST_CASE("DescriptorValidation: MidiEffect without audio output is valid",
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

TEST_CASE("DescriptorValidation: reverse-DNS bundle_id passes",
          "[format][descriptor-validation]") {
    auto d = well_formed_effect();
    d.bundle_id = "com.example.sub.plugin";
    auto issues = validate_descriptor(d);
    for (const auto& i : issues) {
        REQUIRE(i.field != "bundle_id");
    }
}
