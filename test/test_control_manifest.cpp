#include <pulp/inspect/control_manifest.hpp>
#include <pulp/runtime/crypto.hpp>

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <set>
#include <string>

using namespace pulp::inspect;

namespace {

ControlManifest developer_manifest() {
    ControlManifest manifest;
    manifest.profile = ControlBuildProfile::DeveloperLocal;
    manifest.target = "Example";
    manifest.product_name = "Example Product";
    manifest.bundle_id = "com.example.product";
    manifest.build_id = "build:0123456789abcdef0123456789abcdef";
    manifest.endpoint_included = true;
    manifest.capabilities = {
        InspectorCapability::StateRead,
        InspectorCapability::SessionDescribe,
    };
    return manifest;
}

bool schema_strings_are_bounded(choc::value::ValueView value) {
    if (value.isObject()) {
        const auto type = value["type"];
        if (type.isString() && type.getString() == "string" &&
            !value.hasObjectMember("maxLength") &&
            !value.hasObjectMember("pattern"))
            return false;
        for (std::uint32_t index = 0; index < value.size(); ++index) {
            if (!schema_strings_are_bounded(
                    value.getObjectMemberAt(index).value))
                return false;
        }
    } else if (value.isArray()) {
        for (std::uint32_t index = 0; index < value.size(); ++index) {
            if (!schema_strings_are_bounded(value[index])) return false;
        }
    }
    return true;
}

bool required_output_identifiers_are_nonempty(choc::value::ValueView value) {
    if (value.isObject()) {
        const auto required = value["required"];
        const auto properties = value["properties"];
        if (required.isArray() && properties.isObject()) {
            static const std::set<std::string_view> semantic_identifiers{
                "artifact_id", "build_id", "id", "lease_id", "out_path",
                "plugin_id", "receipt_id", "stream_id",
            };
            for (std::uint32_t index = 0; index < required.size(); ++index) {
                if (!required[index].isString()) continue;
                const std::string_view name = required[index].getString();
                if (!semantic_identifiers.contains(name)) continue;
                const auto property = properties[name];
                if (!property.isObject() || !property["type"].isString())
                    return false;
                if (property["type"].getString() != "string") continue;
                if (!property["minLength"].isInt32() &&
                    !property["minLength"].isInt64())
                    return false;
                const auto minimum = property["minLength"].isInt32()
                    ? static_cast<std::int64_t>(
                          property["minLength"].getInt32())
                    : property["minLength"].getInt64();
                if (minimum < 1) return false;
            }
        }
        for (std::uint32_t index = 0; index < value.size(); ++index) {
            if (!required_output_identifiers_are_nonempty(
                    value.getObjectMemberAt(index).value))
                return false;
        }
    } else if (value.isArray()) {
        for (std::uint32_t index = 0; index < value.size(); ++index) {
            if (!required_output_identifiers_are_nonempty(value[index]))
                return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("control manifest canonical round trip is reproducible",
          "[inspect][control-manifest]") {
    const auto manifest = developer_manifest();
    const auto canonical = serialize_control_manifest(manifest);
    REQUIRE_FALSE(canonical.empty());
    REQUIRE(canonical.find("dev.pulp.control/artifact-manifest@1") !=
            std::string::npos);
    REQUIRE(canonical.find("dev.pulp.instance/read@1") <
            canonical.find("dev.pulp.state/read@1"));

    ControlManifestDiagnostics diagnostics;
    const auto parsed = parse_control_manifest(canonical, &diagnostics);
    INFO(diagnostics.error);
    REQUIRE(parsed.has_value());
    REQUIRE(serialize_control_manifest(*parsed) == canonical);
    REQUIRE(control_manifest_digest(*parsed) ==
            control_manifest_digest(manifest));

    auto reordered = manifest;
    std::reverse(reordered.capabilities.begin(), reordered.capabilities.end());
    REQUIRE(serialize_control_manifest(reordered) == canonical);
    REQUIRE(control_manifest_digest(reordered) ==
            control_manifest_digest(manifest));
}

TEST_CASE("control manifest rejects unknown fields and version downgrades",
          "[inspect][control-manifest]") {
    const auto canonical = serialize_control_manifest(developer_manifest());

    auto unknown = canonical;
    const auto closing = unknown.rfind('\n');
    REQUIRE(closing != std::string::npos);
    unknown.insert(closing - 1, ",\n  \"future_authority\": true");
    ControlManifestDiagnostics diagnostics;
    REQUIRE_FALSE(parse_control_manifest(unknown, &diagnostics).has_value());
    REQUIRE(diagnostics.code == ControlManifestError::UnknownField);
    REQUIRE(control_manifest_error_id(diagnostics.code) ==
            "manifest.unknown-field");
    REQUIRE(diagnostics.error.find("unknown field") != std::string::npos);
    REQUIRE(diagnostics.unknown_fields ==
            std::vector<std::string>{"future_authority"});

    auto downgraded = canonical;
    const auto version = downgraded.find("\"schema_version\": 1");
    REQUIRE(version != std::string::npos);
    downgraded.replace(version, std::string("\"schema_version\": 1").size(),
                       "\"schema_version\": 0");
    REQUIRE_FALSE(parse_control_manifest(downgraded, &diagnostics).has_value());
    REQUIRE(diagnostics.code == ControlManifestError::VersionDowngrade);
    REQUIRE(diagnostics.error.find("downgrade") != std::string::npos);

    auto future = canonical;
    const auto future_version = future.find("\"schema_version\": 1");
    future.replace(future_version, std::string("\"schema_version\": 1").size(),
                   "\"schema_version\": 2");
    REQUIRE_FALSE(parse_control_manifest(future, &diagnostics).has_value());
    REQUIRE(diagnostics.code == ControlManifestError::VersionTooNew);
    REQUIRE(diagnostics.error.find("newer") != std::string::npos);
}

TEST_CASE("control denial reasons are stable protocol identifiers",
          "[inspect][control-manifest]") {
    REQUIRE(control_denial_reason_id(ControlDenialReason::NotBuilt) ==
            "not-built");
    REQUIRE(control_denial_reason_id(ControlDenialReason::ClientNotGranted) ==
            "client-not-granted");
    REQUIRE(control_denial_reason_id(ControlDenialReason::PublicationMismatch) ==
            "publication-mismatch");
}

TEST_CASE("control permission equation fails closed at every term",
          "[inspect][control-manifest]") {
    ControlPermissionInputs inputs;
    const struct {
        bool ControlPermissionInputs::*term;
        ControlDenialReason denial;
    } terms[] = {
        {&ControlPermissionInputs::implemented,
         ControlDenialReason::NotImplemented},
        {&ControlPermissionInputs::built, ControlDenialReason::NotBuilt},
        {&ControlPermissionInputs::host_available,
         ControlDenialReason::HostUnavailable},
        {&ControlPermissionInputs::activated,
         ControlDenialReason::NotActivated},
        {&ControlPermissionInputs::policy_eligible,
         ControlDenialReason::PolicyIneligible},
        {&ControlPermissionInputs::client_granted,
         ControlDenialReason::ClientNotGranted},
        {&ControlPermissionInputs::session_live,
         ControlDenialReason::SessionNotLive},
    };

    for (const auto& term : terms) {
        const auto decision = evaluate_control_permission(inputs);
        CHECK_FALSE(decision.allowed);
        REQUIRE(decision.denial.has_value());
        CHECK(*decision.denial == term.denial);
        inputs.*(term.term) = true;
    }
    const auto allowed = evaluate_control_permission(inputs);
    CHECK(allowed.allowed);
    CHECK_FALSE(allowed.denial.has_value());

    inputs.client_granted = false;
    const auto denied = evaluate_control_permission(inputs);
    REQUIRE(denied.denial.has_value());
    CHECK(*denied.denial == ControlDenialReason::ClientNotGranted);
}

TEST_CASE("control profile validation is fail closed",
          "[inspect][control-manifest]") {
    std::string error;
    ControlManifest stripped;
    stripped.target = "Production";
    stripped.product_name = "Production";
    stripped.bundle_id = "com.example.production";
    stripped.build_id = "build:11111111111111111111111111111111";
    REQUIRE(validate_control_manifest(stripped, error));

    stripped.endpoint_included = true;
    REQUIRE_FALSE(validate_control_manifest(stripped, error));
    REQUIRE((error.find("endpoint_included") != std::string::npos ||
             error.find("production-stripped") != std::string::npos));

    auto support = developer_manifest();
    support.profile = ControlBuildProfile::SupportDiagnostics;
    support.capabilities.push_back(InspectorCapability::StateWrite);
    support.capabilities.push_back(InspectorCapability::SessionControl);
    REQUIRE_FALSE(validate_control_manifest(support, error));
    REQUIRE(error.find("support-diagnostics") != std::string::npos);

    auto eval = developer_manifest();
    eval.capabilities.push_back(InspectorCapability::RuntimeEval);
    eval.capabilities.push_back(InspectorCapability::SessionControl);
    eval.unsafe_runtime_eval_acknowledged = true;
    REQUIRE_FALSE(validate_control_manifest(eval, error));
    REQUIRE(error.find("research-unsafe") != std::string::npos);
    eval.profile = ControlBuildProfile::ResearchUnsafe;
    REQUIRE(validate_control_manifest(eval, error));
}

TEST_CASE("artifact and registry identity participate in manifest consent digest",
          "[inspect][control-manifest]") {
    const auto original = developer_manifest();
    auto rebuilt = original;
    rebuilt.build_id = "build:22222222222222222222222222222222";
    REQUIRE(control_manifest_digest(original) != control_manifest_digest(rebuilt));

    auto stale_registry = original;
    stale_registry.registry_digest = "sha256:stale-registry";
    std::string error;
    REQUIRE_FALSE(validate_control_manifest(stale_registry, error));
    REQUIRE(error.find("registry_digest") != std::string::npos);

    const auto manifest_digest = control_manifest_digest(original);
    const std::string artifact_a(64, 'a');
    const std::string artifact_b(64, 'b');
    REQUIRE(control_consent_identity(manifest_digest, artifact_a) !=
            control_consent_identity(manifest_digest, artifact_b));
}

TEST_CASE("manifest validation returns typed semantic failures",
          "[inspect][control-manifest]") {
    auto manifest = developer_manifest();
    manifest.profile = static_cast<ControlBuildProfile>(255);
    auto result = validate_control_manifest_detailed(manifest);
    CHECK_FALSE(result.valid);
    CHECK(result.code == ControlManifestError::InvalidProfile);
    CHECK(serialize_control_manifest(manifest).empty());

    manifest = developer_manifest();
    manifest.build_id = "sha256:not-a-build-identity";
    result = validate_control_manifest_detailed(manifest);
    CHECK(result.code == ControlManifestError::InvalidIdentity);

    ControlManifest stripped;
    stripped.target = "LegacyCompatible";
    stripped.product_name = "Legacy Compatible";
    stripped.build_id = "build:55555555555555555555555555555555";
    CHECK(validate_control_manifest_detailed(stripped).valid);
    manifest.bundle_id.clear();
    result = validate_control_manifest_detailed(manifest);
    CHECK(result.code == ControlManifestError::InvalidIdentity);

    manifest = developer_manifest();
    manifest.target = std::string(129, 't');
    CHECK(validate_control_manifest_detailed(manifest).code ==
          ControlManifestError::InvalidIdentity);
    manifest = developer_manifest();
    manifest.product_name = std::string(257, 'p');
    CHECK(validate_control_manifest_detailed(manifest).code ==
          ControlManifestError::InvalidIdentity);
}

TEST_CASE("canonical manifest validation requires controller authority",
          "[inspect][control-manifest]") {
    auto manifest = developer_manifest();
    manifest.capabilities.push_back(InspectorCapability::StateWrite);
    std::string error;
    REQUIRE_FALSE(validate_control_manifest(manifest, error));
    REQUIRE(error.find("dev.pulp.session/control@1") != std::string::npos);

    manifest.capabilities.push_back(InspectorCapability::SessionControl);
    auto json = serialize_control_manifest(manifest);
    const std::string controller =
        "\"dev.pulp.session/control@1\", ";
    const auto controller_position = json.find(controller);
    REQUIRE(controller_position != std::string::npos);
    json.erase(controller_position, controller.size());
    ControlManifestDiagnostics diagnostics;
    REQUIRE_FALSE(parse_control_manifest(json, &diagnostics).has_value());
    REQUIRE(diagnostics.code ==
            ControlManifestError::MissingCapabilityDependency);
    REQUIRE(control_manifest_error_id(diagnostics.code) ==
            "manifest.missing-capability-dependency");
}

TEST_CASE("control registry projects capability and operation metadata",
          "[inspect][control-manifest]") {
    const auto registry = serialize_control_registry();
    const auto registry_digest = pulp::runtime::sha256_hex(registry);
    INFO("registry digest: " << registry_digest);
    REQUIRE(registry_digest == kControlRegistryDigest);
    REQUIRE(registry.find("dev.pulp.state/parameter-gesture@1") !=
            std::string::npos);
    REQUIRE(registry.find("\"risk\":\"mutating\"") != std::string::npos);
    REQUIRE(registry.find("\"risk\":\"high-risk-mutation\"") !=
            std::string::npos);
    REQUIRE(registry.find("\"risk\":\"critical\"") != std::string::npos);
    REQUIRE(registry.find("\"executor\":\"host-main\"") !=
            std::string::npos);
    REQUIRE(registry.find("\"evidence\":\"receipt\"") !=
            std::string::npos);
    REQUIRE(registry.find("State.setParameter") != std::string::npos);
    REQUIRE(registry.find("\"input_schema_id\":\"dev.pulp.schema/") !=
            std::string::npos);
    REQUIRE(registry.find("\"output_schema_id\":\"dev.pulp.schema/") !=
            std::string::npos);
    REQUIRE(registry.find("\"input_schema\":{\"$schema\"") !=
            std::string::npos);
    REQUIRE(registry.find("\"result_kind\":\"receipt\"") !=
            std::string::npos);
    REQUIRE(registry.find("\"input_schema_digest\":") != std::string::npos);
    REQUIRE(registry.find("\"maximum\":4294967295") !=
            std::string::npos);
    REQUIRE(registry.find("\"required_build_feature\":") !=
            std::string::npos);
    REQUIRE(registry.find("\"runtime_contexts\":") != std::string::npos);
    REQUIRE(registry.find("\"host_tiers\":") != std::string::npos);
    REQUIRE(registry.find("\"cancellation\":") != std::string::npos);
    REQUIRE(registry.find("\"compatibility\":") != std::string::npos);

    std::size_t capability_count = 0;
    for (const auto& capability : inspector_capability_registry()) {
        if (capability.capability != InspectorCapability::Unavailable)
            ++capability_count;
    }
    REQUIRE(control_operation_registry().size() == capability_count);
    std::set<std::string_view> operation_ids;
    std::set<std::string_view> schema_ids;
    for (const auto& operation : control_operation_registry()) {
        INFO("operation: " << operation.id);
        CHECK(operation.id == capability_contract_id(operation.capability));
        CHECK(operation_ids.insert(operation.id).second);
        CHECK(schema_ids.insert(operation.input_schema_id).second);
        CHECK(schema_ids.insert(operation.output_schema_id).second);
        CHECK(choc::json::parse(operation.input_schema_json).isObject());
        CHECK(choc::json::parse(operation.output_schema_json).isObject());
        CHECK(operation.input_schema_json.find("additionalProperties") !=
              std::string_view::npos);
        CHECK_FALSE(operation.result_kind.empty());
        CHECK(schema_strings_are_bounded(
            choc::json::parse(operation.input_schema_json)));
        CHECK(schema_strings_are_bounded(
            choc::json::parse(operation.output_schema_json)));
        CHECK(required_output_identifiers_are_nonempty(
            choc::json::parse(operation.output_schema_json)));
        if (operation.capability == InspectorCapability::UiInput ||
            operation.capability == InspectorCapability::TestInput) {
            CHECK(operation.input_schema_json.find("\"oneOf\"") !=
                  std::string_view::npos);
            CHECK(operation.input_schema_json.find(
                      "\"event\":{\"type\":\"object\"}") ==
                  std::string_view::npos);
        }
        if (operation.capability == InspectorCapability::AuthoringTweaks)
        {
            CHECK(operation.input_schema_json.find("\"propertyNames\"") !=
                  std::string_view::npos);
            CHECK(operation.input_schema_json.find("\"anchor_id\"") !=
                  std::string_view::npos);
            CHECK(operation.input_schema_json.find("\"allOf\"") !=
                  std::string_view::npos);
            CHECK(operation.input_schema_json.find(
                      "\"highlight_node_id\":{\"maxLength\":256,\"minLength\":1,\"type\":\"string\",\"x-pulp-maxUtf8Bytes\":256}") !=
                  std::string_view::npos);
        }
        if (operation.capability == InspectorCapability::CaptureImage) {
            CHECK(operation.input_schema_json.find("\"oneOf\"") !=
                  std::string_view::npos);
            CHECK(operation.input_schema_json.find("\"const\":\"node\"") !=
                  std::string_view::npos);
        }
        if (operation.capability == InspectorCapability::TestInput) {
            CHECK(operation.input_schema_json.find("\"maximum\":400") !=
                  std::string_view::npos);
            CHECK(operation.input_schema_json.find("\"minimum\":20") !=
                  std::string_view::npos);
        }
        if (operation.capability == InspectorCapability::TelemetryStream) {
            CHECK(operation.input_schema_json.find("\"maxItems\":32") !=
                  std::string_view::npos);
            CHECK(operation.input_schema_json.find("\"uniqueItems\":true") !=
                  std::string_view::npos);
        }
        if (operation.capability == InspectorCapability::TraceControl) {
            CHECK(operation.input_schema_json.find(
                      "\"const\":\"motion-start-trace\"") !=
                  std::string_view::npos);
            CHECK(operation.input_schema_json.find("\"maxItems\":32") !=
                  std::string_view::npos);
            CHECK(operation.output_schema_json.find("\"trace_id\"") !=
                  std::string_view::npos);
            CHECK(operation.output_schema_json.find(
                      "\"required\":[\"action\",\"receipt_id\",\"applied\",\"trace_id\"]") !=
                  std::string_view::npos);
        }
        if (operation.capability ==
            InspectorCapability::TraceSessionControl) {
            CHECK(operation.input_schema_json.find("\"maximum\":512") !=
                  std::string_view::npos);
            CHECK(operation.input_schema_json.find(
                      "\"x-pulp-maxUtf8Bytes\":128") !=
                  std::string_view::npos);
            CHECK(operation.output_schema_json.find("\"out_path\"") !=
                  std::string_view::npos);
        }
        if (operation.capability == InspectorCapability::RuntimeEval) {
            CHECK(operation.input_schema_json.find(
                      "\"x-pulp-maxUtf8Bytes\":65536") !=
                  std::string_view::npos);
            CHECK(operation.input_schema_json.find("\\\\u0000") !=
                  std::string_view::npos);
        }
    }
    REQUIRE(registry.find("dev.pulp.render/offline@1") != std::string::npos);
    REQUIRE(registry.find("dev.pulp.ui/input@1") != std::string::npos);
    REQUIRE(registry.find("dev.pulp.runtime/reload@1") != std::string::npos);
    REQUIRE(registry.find("dev.pulp.artifact/read@1") != std::string::npos);
}
