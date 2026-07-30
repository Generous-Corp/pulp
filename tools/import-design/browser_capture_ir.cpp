#include "browser_capture_ir.hpp"
#include "browser_capture_limits.hpp"

#include <pulp/runtime/crypto.hpp>
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

namespace pulp::import_design {

namespace fs = std::filesystem;
using pulp::view::DesignIR;
using pulp::view::IRAssetRef;
using pulp::view::IRTokenIdentity;
using pulp::view::NodeRenderMode;

namespace {

std::string string_member(const choc::value::ValueView& object,
                          const char* key) {
    if (!object.isObject() || !object.hasObjectMember(key) ||
        !object[key].isString())
        return {};
    return object[key].toString();
}

double number_member(const choc::value::ValueView& object,
                     const char* key,
                     double fallback = 0.0) {
    if (!object.isObject() || !object.hasObjectMember(key))
        return fallback;
    return object[key].getWithDefault<double>(fallback);
}

bool validate_reference_geometry(double logical_width,
                                 double logical_height,
                                 double dpr,
                                 std::string& error) {
    if (dpr != static_cast<double>(
                   browser_capture::kDefaultDeviceScaleFactor)) {
        error = "browser capture reference requires DPR 2";
        return false;
    }
    const auto valid_logical_dimension = [](double value) {
        return std::isfinite(value) && value > 0.0 &&
               std::trunc(value) == value &&
               value <=
                   browser_capture::kMaximumLogicalViewportDimension;
    };
    if (!valid_logical_dimension(logical_width) ||
        !valid_logical_dimension(logical_height)) {
        error =
            "browser capture reference requires finite positive integer "
            "logical dimensions no larger than 8192";
        return false;
    }
    if (!browser_capture::viewport_within_capture_limits(
            static_cast<int>(logical_width),
            static_cast<int>(logical_height),
            browser_capture::kDefaultDeviceScaleFactor)) {
        error =
            "browser capture reference exceeds the 64 megapixel safety limit";
        return false;
    }
    return true;
}

choc::value::ValueView object_member(const choc::value::ValueView& object,
                                     const char* key) {
    if (!object.isObject() || !object.hasObjectMember(key) ||
        !object[key].isObject())
        return {};
    return object[key];
}

bool load_token_report(const fs::path& path,
                       DesignIR& ir,
                       int expected_colors,
                       int expected_dimensions,
                       int expected_strings,
                       std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "could not open browser token report: " + path.string();
        return false;
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();
    choc::value::Value report;
    try {
        report = choc::json::parse(bytes.str());
    } catch (const std::exception& e) {
        error = std::string("invalid browser token JSON: ") + e.what();
        return false;
    }
    if (!report.isObject() ||
        string_member(report, "schema") != "pulp-browser-tokens-v1" ||
        number_member(report, "version", 0.0) != 1.0) {
        error =
            "unsupported browser token report (expected pulp-browser-tokens-v1 version 1)";
        return false;
    }

    const auto colors = object_member(report, "colors");
    for (uint32_t i = 0; i < colors.size(); ++i) {
        const auto member = colors.getObjectMemberAt(i);
        if (member.value.isString())
            ir.tokens.colors[std::string(member.name)] =
                member.value.toString();
    }
    const auto dimensions = object_member(report, "dimensions");
    for (uint32_t i = 0; i < dimensions.size(); ++i) {
        const auto member = dimensions.getObjectMemberAt(i);
        if (member.value.isFloat() || member.value.isInt())
            ir.tokens.dimensions[std::string(member.name)] =
                static_cast<float>(member.value.getWithDefault<double>(0.0));
    }
    const auto strings = object_member(report, "strings");
    for (uint32_t i = 0; i < strings.size(); ++i) {
        const auto member = strings.getObjectMemberAt(i);
        if (member.value.isString())
            ir.tokens.strings[std::string(member.name)] =
                member.value.toString();
    }
    if (expected_colors < 0 || expected_dimensions < 0 ||
        expected_strings < 0 ||
        static_cast<int>(ir.tokens.colors.size()) != expected_colors ||
        static_cast<int>(ir.tokens.dimensions.size()) != expected_dimensions ||
        static_cast<int>(ir.tokens.strings.size()) != expected_strings) {
        error = "browser token counts do not match capture envelope";
        return false;
    }
    const auto identities = object_member(report, "source_identity");
    for (uint32_t i = 0; i < identities.size(); ++i) {
        const auto member = identities.getObjectMemberAt(i);
        if (!member.value.isObject()) continue;
        IRTokenIdentity identity;
        identity.source_id = string_member(member.value, "source_id");
        identity.source_collection =
            string_member(member.value, "source_collection");
        identity.source_mode = string_member(member.value, "source_mode");
        identity.source_adapter =
            string_member(member.value, "source_adapter");
        ir.tokens.source_identity[std::string(member.name)] =
            std::move(identity);
    }
    return true;
}

bool validate_semantic_report(const fs::path& path,
                              int expected_candidates,
                              int expected_resolved,
                              int expected_unresolved,
                              std::string& error) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    choc::value::Value report;
    try {
        report = choc::json::parse(bytes.str());
    } catch (const std::exception& e) {
        error = std::string("invalid browser semantic JSON: ") + e.what();
        return false;
    }
    const auto summary = object_member(report, "summary");
    if (!report.isObject() ||
        string_member(report, "schema") != "pulp-browser-semantics-v1" ||
        number_member(report, "version", 0.0) != 1.0 ||
        !summary.isObject()) {
        error =
            "unsupported browser semantic report "
            "(expected pulp-browser-semantics-v1 version 1 with summary)";
        return false;
    }
    const auto candidates =
        static_cast<int>(number_member(summary, "candidates", -1.0));
    const auto resolved =
        static_cast<int>(number_member(summary, "resolved", -1.0));
    const auto unresolved =
        static_cast<int>(number_member(summary, "unresolved", -1.0));
    if (candidates < 0 || resolved < 0 || unresolved < 0 ||
        candidates != resolved + unresolved ||
        candidates != expected_candidates || resolved != expected_resolved ||
        unresolved != expected_unresolved) {
        error = "browser semantic summary does not match capture envelope";
        return false;
    }
    return true;
}

bool validate_png_header(const fs::path& path,
                         int expected_width,
                         int expected_height,
                         std::string& error) {
    std::ifstream input(path, std::ios::binary);
    unsigned char header[24]{};
    input.read(reinterpret_cast<char*>(header), sizeof(header));
    constexpr unsigned char signature[] =
        {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (input.gcount() != static_cast<std::streamsize>(sizeof(header)) ||
        !std::equal(std::begin(signature), std::end(signature), header)) {
        error = "browser reference is not a PNG";
        return false;
    }
    auto be32 = [&](int offset) {
        return (static_cast<int>(header[offset]) << 24) |
               (static_cast<int>(header[offset + 1]) << 16) |
               (static_cast<int>(header[offset + 2]) << 8) |
               static_cast<int>(header[offset + 3]);
    };
    if (be32(16) != expected_width || be32(20) != expected_height) {
        error = "browser reference PNG dimensions do not match capture envelope";
        return false;
    }
    return true;
}

std::string file_sha256(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    return pulp::runtime::sha256_hex(bytes.data(), bytes.size());
}

bool is_sha256(std::string_view value) {
    return value.size() == 64 &&
           std::all_of(
               value.begin(), value.end(), [](unsigned char character) {
                   return (character >= '0' && character <= '9') ||
                          (character >= 'a' && character <= 'f');
               });
}

bool integer_member(const choc::value::ValueView& object,
                    const char* key,
                    int minimum,
                    int maximum,
                    int& value) {
    if (!object.isObject() || !object.hasObjectMember(key) ||
        (!object[key].isInt() && !object[key].isFloat())) {
        return false;
    }
    const auto number = object[key].getWithDefault<double>(
        std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(number) || std::trunc(number) != number ||
        number < minimum || number > maximum) {
        return false;
    }
    value = static_cast<int>(number);
    return true;
}

bool validate_interaction_report(
    const fs::path& path,
    const choc::value::ValueView& provenance,
    std::string& plan_sha256,
    std::string& report_sha256,
    int& action_count,
    std::string& error) {
    if (string_member(provenance, "schema") !=
            "pulp-browser-interactions-v1" ||
        number_member(provenance, "version", 0.0) != 1.0) {
        error =
            "unsupported browser interaction provenance "
            "(expected pulp-browser-interactions-v1 version 1)";
        return false;
    }
    plan_sha256 = string_member(provenance, "plan_sha256");
    report_sha256 = string_member(provenance, "report_sha256");
    if (!is_sha256(plan_sha256) || !is_sha256(report_sha256)) {
        error =
            "browser interaction provenance requires lowercase SHA-256 "
            "plan and report hashes";
        return false;
    }
    if (!integer_member(provenance, "action_count", 1, 32, action_count)) {
        error =
            "browser interaction provenance requires an action_count from 1 to 32";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "could not open browser interaction report: " + path.string();
        return false;
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();
    const auto report_bytes = bytes.str();
    if (pulp::runtime::sha256_hex(report_bytes) != report_sha256) {
        error =
            "browser interaction report hash does not match capture envelope";
        return false;
    }
    choc::value::Value report;
    try {
        report = choc::json::parse(report_bytes);
    } catch (const std::exception& exception) {
        error = std::string("invalid browser interaction JSON: ") +
                exception.what();
        return false;
    }
    int report_action_count = 0;
    if (!report.isObject() ||
        string_member(report, "schema") !=
            "pulp-browser-interactions-v1" ||
        number_member(report, "version", 0.0) != 1.0 ||
        string_member(report, "plan_sha256") != plan_sha256 ||
        !integer_member(
            report, "action_count", 1, 32, report_action_count) ||
        !report.hasObjectMember("actions") ||
        !report["actions"].isArray() ||
        report["actions"].size() !=
            static_cast<uint32_t>(report_action_count) ||
        report_action_count != action_count) {
        error =
            "browser interaction report does not match capture provenance";
        return false;
    }
    const auto actions = report["actions"];
    for (uint32_t index = 0; index < actions.size(); ++index) {
        const auto action = actions[static_cast<int>(index)];
        const auto name = string_member(action, "action");
        const bool allowed =
            name == "click" || name == "type" || name == "wait-for" ||
            name == "wait-ms";
        if (!action.isObject() || !allowed ||
            string_member(action, "status") != "completed" ||
            action.hasObjectMember("text") ||
            action.hasObjectMember("text_sha256")) {
            error =
                "browser interaction report contains invalid or private action evidence";
            return false;
        }
    }
    return true;
}

std::optional<fs::path> contained_sidecar(const fs::path& envelope,
                                          std::string_view authored,
                                          std::string& error) {
    if (authored.empty()) {
        error = "capture envelope names an empty sidecar path";
        return std::nullopt;
    }
    const fs::path relative{authored};
    if (relative.is_absolute()) {
        error = "capture envelope sidecar path must be relative: " +
                relative.string();
        return std::nullopt;
    }

    std::error_code ec;
    const auto base = fs::weakly_canonical(envelope.parent_path(), ec);
    if (ec) {
        error = "could not resolve capture directory: " + ec.message();
        return std::nullopt;
    }
    const auto candidate = fs::weakly_canonical(base / relative, ec);
    if (ec) {
        error = "could not resolve capture sidecar '" + relative.string() +
                "': " + ec.message();
        return std::nullopt;
    }
    const auto rel = fs::relative(candidate, base, ec);
    if (ec || rel.empty() || rel.is_absolute() ||
        *rel.begin() == fs::path("..")) {
        error = "capture sidecar escapes the capture directory: " +
                relative.string();
        return std::nullopt;
    }
    if (!fs::is_regular_file(candidate, ec) || ec) {
        error = "capture sidecar is missing or not a regular file: " +
                candidate.string();
        return std::nullopt;
    }
    return candidate;
}

}  // namespace

BrowserCaptureIrResult lower_browser_capture_to_ir(
    const fs::path& envelope_path,
    const BrowserCaptureIrOptions& options) {
    BrowserCaptureIrResult result;

    std::ifstream input(envelope_path, std::ios::binary);
    if (!input) {
        result.error = "could not open browser capture envelope: " +
                       envelope_path.string();
        return result;
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();

    choc::value::Value envelope;
    try {
        envelope = choc::json::parse(bytes.str());
    } catch (const std::exception& e) {
        result.error = std::string("invalid browser capture JSON: ") + e.what();
        return result;
    }
    if (!envelope.isObject() ||
        string_member(envelope, "schema") != "pulp-browser-capture-v1" ||
        number_member(envelope, "version", 0.0) != 1.0) {
        result.error =
            "unsupported browser capture envelope (expected pulp-browser-capture-v1 version 1)";
        return result;
    }
    const auto provenance = object_member(envelope, "provenance");
    if (string_member(provenance, "capture_method") != "chromium-cdp" ||
        !envelope.hasObjectMember("documents") ||
        !envelope["documents"].isArray() || envelope["documents"].size() == 0 ||
        !envelope.hasObjectMember("states") ||
        !envelope["states"].isArray() || envelope["states"].size() == 0) {
        result.error =
            "browser capture envelope is missing required provenance, "
            "documents, or states";
        return result;
    }

    const auto reference = object_member(envelope, "reference");
    const std::string reference_id = string_member(reference, "asset_id");
    const std::string reference_path = string_member(reference, "path");
    const double logical_width = number_member(reference, "logical_width");
    const double logical_height = number_member(reference, "logical_height");
    const double dpr = number_member(reference, "device_scale_factor");
    if (reference_id.empty()) {
        result.error =
            "browser capture reference requires a non-empty asset_id";
        return result;
    }
    if (!validate_reference_geometry(
            logical_width, logical_height, dpr, result.error))
        return result;

    const auto documents = envelope["documents"];
    for (uint32_t i = 0; i < documents.size(); ++i) {
        const auto document = documents[static_cast<int>(i)];
        if (!document.isObject() || string_member(document, "id").empty()) {
            result.error =
                "browser capture document requires a non-empty id";
            return result;
        }
    }
    const auto states = envelope["states"];
    for (uint32_t i = 0; i < states.size(); ++i) {
        const auto state = states[static_cast<int>(i)];
        if (!state.isObject() || string_member(state, "name").empty() ||
            string_member(state, "reference_asset_id") != reference_id) {
            result.error =
                "browser capture state does not reference the canonical asset";
            return result;
        }
    }
    auto reference_png =
        contained_sidecar(envelope_path, reference_path, result.error);
    if (!reference_png) return result;

    const auto semantics = object_member(envelope, "semantics");
    const std::string semantic_path = string_member(semantics, "report");
    auto semantic_report =
        contained_sidecar(envelope_path, semantic_path, result.error);
    if (!semantic_report) return result;
    const int candidate_count =
        static_cast<int>(number_member(semantics, "candidate_count", -1.0));
    const int resolved_count =
        static_cast<int>(number_member(semantics, "resolved_count", -1.0));
    const int unresolved_count =
        static_cast<int>(number_member(semantics, "unresolved_count", -1.0));
    if (!validate_semantic_report(
            *semantic_report, candidate_count, resolved_count,
            unresolved_count, result.error))
        return result;
    const auto tokens = object_member(envelope, "tokens");
    const std::string token_path = string_member(tokens, "report");
    auto token_report =
        contained_sidecar(envelope_path, token_path, result.error);
    if (!token_report) return result;

    std::optional<fs::path> interaction_report;
    std::string interaction_plan_sha256;
    std::string interaction_report_sha256;
    int interaction_action_count = 0;
    const bool has_interaction_member =
        provenance.hasObjectMember("interactions");
    if (options.require_interaction_report && !has_interaction_member) {
        result.error =
            "browser capture requested interactions but omitted interaction provenance";
        return result;
    }
    if (has_interaction_member) {
        if (!provenance["interactions"].isObject()) {
            result.error = "browser capture interaction provenance must be an object";
            return result;
        }
        const auto interactions = provenance["interactions"];
        interaction_report = contained_sidecar(
            envelope_path, string_member(interactions, "report"),
            result.error);
        if (!interaction_report) return result;
        if (!validate_interaction_report(
                *interaction_report, interactions,
                interaction_plan_sha256, interaction_report_sha256,
                interaction_action_count, result.error)) {
            return result;
        }
    }

    IRAssetRef backing;
    backing.asset_id = reference_id;
    backing.original_uri = "pulp-capture:///" + reference_path;
    backing.local_path = reference_png->string();
    backing.mime = "image/png";

    int matching_reference_assets = 0;
    if (envelope.hasObjectMember("assets") && envelope["assets"].isArray()) {
        const auto assets = envelope["assets"];
        for (uint32_t i = 0; i < assets.size(); ++i) {
            const auto asset = assets[static_cast<int>(i)];
            if (!asset.isObject() || string_member(asset, "id") != reference_id)
                continue;
            ++matching_reference_assets;
            if (string_member(asset, "path") != reference_path ||
                string_member(asset, "mime_type") != "image/png") {
                result.error =
                    "browser reference asset metadata does not match reference";
                return result;
            }
            backing.content_hash = string_member(asset, "sha256");
            const double width_px = number_member(asset, "width_px", -1.0);
            const double height_px = number_member(asset, "height_px", -1.0);
            const double expected_width_px = logical_width * dpr;
            const double expected_height_px = logical_height * dpr;
            if (!std::isfinite(width_px) || !std::isfinite(height_px) ||
                width_px <= 0.0 || height_px <= 0.0 ||
                std::trunc(width_px) != width_px ||
                std::trunc(height_px) != height_px ||
                width_px != expected_width_px ||
                height_px != expected_height_px ||
                width_px > std::numeric_limits<int>::max() ||
                height_px > std::numeric_limits<int>::max()) {
                result.error =
                    "browser reference asset pixel dimensions do not match "
                    "logical dimensions and DPR";
                return result;
            }
            backing.width = static_cast<int>(width_px);
            backing.height = static_cast<int>(height_px);
        }
    }
    if (matching_reference_assets != 1 || !backing.width || !backing.height ||
        backing.content_hash.size() != 64) {
        result.error = "browser reference asset metadata is incomplete";
        return result;
    }
    if (file_sha256(*reference_png) != backing.content_hash) {
        result.error = "browser reference PNG hash does not match capture envelope";
        return result;
    }
    if (!validate_png_header(
            *reference_png, *backing.width, *backing.height, result.error))
        return result;

    DesignIR ir;
    ir.source = options.source;
    ir.source_file = options.source_file;
    ir.capture_method = "chromium-cdp";
    ir.source_adapter = "browser-capture";
    ir.source_version = "pulp-browser-capture-v1";
    ir.asset_manifest.assets.push_back(std::move(backing));
    if (!load_token_report(
            *token_report, ir,
            static_cast<int>(number_member(tokens, "color_count", -1.0)),
            static_cast<int>(number_member(tokens, "dimension_count", -1.0)),
            static_cast<int>(number_member(tokens, "string_count", -1.0)),
            result.error))
        return result;

    const auto source = object_member(provenance, "source");
    if (ir.source_file.empty())
        ir.source_file = string_member(source, "entry");
    const auto settle = object_member(provenance, "settle");
    ir.settle_rounds =
        static_cast<int>(number_member(settle, "rounds", 0.0));

    ir.root.type = "frame";
    ir.root.name = "Browser-evaluated HTML";
    ir.root.render_mode = NodeRenderMode::faithful_capture;
    ir.root.capture_asset_id = reference_id;
    ir.root.style.width = static_cast<float>(logical_width);
    ir.root.style.height = static_cast<float>(logical_height);
    ir.root.style.object_fit = "fill";
    ir.root.style.overflow = "hidden";
    ir.root.stable_anchor_id = "browser:root";
    ir.root.anchor_strategy = "adapter";
    ir.root.source_adapter = "browser-capture";
    ir.root.source_version = "pulp-browser-capture-v1";
    // The typed capture_asset_id is the render contract. asset_ref keeps the
    // existing source-agnostic manifest refresh/localization pass aware of the
    // same backing without teaching that utility a browser-specific field.
    ir.root.attributes["asset_ref"] = reference_id;
    // These are evidence identifiers, not filesystem dependencies. Keep them
    // portable when the DesignIR and its localized asset directory move.
    ir.root.attributes["browser_capture_envelope"] =
        "pulp-capture:///capture.json";
    ir.root.attributes["browser_semantic_report"] =
        "pulp-capture:///" + semantic_path;
    ir.root.attributes["browser_token_report"] =
        "pulp-capture:///" + token_path;
    if (interaction_report) {
        ir.root.attributes["browser_interaction_report"] =
            "pulp-capture:///" +
            string_member(provenance["interactions"], "report");
        ir.root.attributes["browser_interaction_report_sha256"] =
            interaction_report_sha256;
        ir.root.attributes["browser_interaction_plan_sha256"] =
            interaction_plan_sha256;
        ir.root.attributes["browser_interaction_action_count"] =
            std::to_string(interaction_action_count);
    }
    ir.root.attributes["browser_semantic_candidates"] =
        std::to_string(static_cast<int>(
            number_member(semantics, "candidate_count", 0.0)));
    ir.root.attributes["browser_semantic_resolved"] =
        std::to_string(static_cast<int>(
            number_member(semantics, "resolved_count", 0.0)));
    ir.root.attributes["browser_semantic_unresolved"] =
        std::to_string(static_cast<int>(
            number_member(semantics, "unresolved_count", 0.0)));
    ir.root.attributes["browser_device_scale_factor"] =
        std::to_string(dpr);

    result.reference_png = *reference_png;
    result.semantic_report = *semantic_report;
    result.interaction_report = std::move(interaction_report);
    result.design_ir = std::move(ir);
    return result;
}

}  // namespace pulp::import_design
