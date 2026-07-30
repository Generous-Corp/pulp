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

/// object_member insists the member is an OBJECT and hands back an empty view
/// for an array, which reads as "absent" — a silent zero rather than an error.
choc::value::ValueView array_member(const choc::value::ValueView& object,
                                    const char* key) {
    if (!object.isObject() || !object.hasObjectMember(key) ||
        !object[key].isArray())
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

/// Lower each bound semantic candidate into a control node beneath the
/// faithful-capture backdrop.
///
/// The capture is a picture; these children are what make it a panel. Every
/// step downstream already exists — design_codegen emits createKnob/createFader
/// /createMeter for any node carrying audio_widget, and the designed-control
/// skin paints value-only geometry so the design underneath is not erased.
///
/// Geometry comes from the author-declared paint box when present. It is NOT
/// the component box: a knob's component includes its caption (116x139.9 where
/// the dial is 116x116), so painting a value ring into it lands the ring ~12px
/// below the dial centre — which renders, looks almost right, and is invisible
/// to pixel-parity and to the component and macro contracts alike.
///
/// Only BOUND candidates become controls. An unbound candidate drives nothing,
/// and a live widget over the design that moves no parameter is worse than
/// leaving that part of the picture alone.
int lower_semantic_controls(const fs::path& path,
                            const DesignIR& ir,
                            pulp::view::IRNode& root,
                            int& undeclared_paint_boxes,
                            std::string& error) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    choc::value::Value report;
    try {
        report = choc::json::parse(bytes.str());
    } catch (const std::exception& e) {
        error = std::string("invalid browser semantic JSON: ") + e.what();
        return -1;
    }
    const auto candidates = array_member(report, "candidates");
    if (!candidates.isArray()) return 0;

    int lowered = 0;
    for (uint32_t i = 0; i < candidates.size(); ++i) {
        const auto candidate = candidates[i];
        if (!candidate.isObject()) continue;
        if (string_member(candidate, "binding_status") != "bound") continue;

        const auto kind = string_member(candidate, "kind");
        auto widget = pulp::view::AudioWidgetType::none;
        if (kind == "knob") widget = pulp::view::AudioWidgetType::knob;
        else if (kind == "fader") widget = pulp::view::AudioWidgetType::fader;
        else if (kind == "meter") widget = pulp::view::AudioWidgetType::meter;
        else continue;  // buttons and unknowns stay part of the backdrop

        const auto data = object_member(candidate, "data_pulp");
        std::string param = string_member(data, "param");
        if (param.empty()) param = string_member(data, "meter");
        if (param.empty()) continue;  // "bound" without a key is not a binding

        // Prefer the declared paint box; fall back to the component box and
        // count it, so an undeclared control is reported rather than silently
        // mispainted. A meter legitimately has no inner paint box — the
        // component IS its track — so the fallback is correct there.
        auto box = object_member(candidate, "paint_bounds");
        if (!box.isObject()) {
            box = object_member(candidate, "bounds");
            ++undeclared_paint_boxes;
        }

        pulp::view::IRNode control;
        control.type = "frame";
        control.name = string_member(candidate, "name");
        control.audio_widget = widget;
        // design_codegen keys the host binding off attributes["binding"] —
        // IRNode::param_key does not exist; that field belongs to the
        // geometry-detected element struct, which is a different lane.
        control.attributes["binding"] = param;
        // The body of this control is the captured bitmap beneath it, so it
        // carries no background of its own and would otherwise fail the
        // has-a-body test that selects the value-only skin -- and be painted
        // over by an opaque default widget body.
        control.attributes["designed_body"] = "capture";
        // The caption is already in the capture. audio_label would draw a
        // second copy on top of it; the name survives on the node for host
        // parameter naming.
        control.audio_label.clear();
        // The design draws the control at a value; the widget must open at the
        // same one or the panel changes the moment it loads. Declared, not
        // inferred: the value lives in a CSS custom property the stylesheet
        // reads, and which property that is differs per design system.
        const auto declared_value = string_member(data, "value");
        if (!declared_value.empty()) {
            try {
                control.audio_default = std::stof(declared_value);
            } catch (const std::exception&) {
                // A malformed value must not take the control down with it;
                // the default stands and the design still renders.
            }
        }
        control.style.position = "absolute";
        control.style.left = static_cast<float>(number_member(box, "left", 0.0));
        control.style.top = static_cast<float>(number_member(box, "top", 0.0));
        control.style.width = static_cast<float>(number_member(box, "width", 0.0));
        control.style.height = static_cast<float>(number_member(box, "height", 0.0));
        // The value layer's colours come from the DESIGN's tokens, not the
        // widget defaults. DesignedControlSkin's header says exactly this, but
        // its call site passes a default-constructed skin whose accent is a
        // hardcoded teal -- so a warm cream panel drew teal arcs and a green
        // meter while the browser capture beside it was perfectly coherent.
        const auto token = [&ir](const char* name) -> std::string {
            const auto it = ir.tokens.colors.find(name);
            return it == ir.tokens.colors.end() ? std::string{} : it->second;
        };
        if (const auto accent = token("css/accent"); !accent.empty())
            control.attributes["design_accent"] = accent;
        if (const auto track = token("css/line-strong"); !track.empty())
            control.attributes["design_track"] = track;
        if (const auto ind = token("css/text-strong"); !ind.empty())
            control.attributes["design_indicator"] = ind;

        root.children.push_back(std::move(control));
        ++lowered;
    }
    return lowered;
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

    // The backdrop alone is a picture. These children are the live controls.
    int undeclared_paint_boxes = 0;
    const int lowered = lower_semantic_controls(
        *semantic_report, ir, ir.root, undeclared_paint_boxes, result.error);
    if (lowered < 0) return result;
    ir.root.attributes["controls_lowered"] = std::to_string(lowered);
    // Surfaced rather than swallowed: an undeclared paint box means the widget
    // was placed on the component box, which for a captioned control paints its
    // value geometry low. Correct for a meter, wrong for a knob, and nothing
    // downstream can tell the difference.
    if (undeclared_paint_boxes > 0)
        ir.root.attributes["controls_without_paint_box"] =
            std::to_string(undeclared_paint_boxes);
    // These are evidence identifiers, not filesystem dependencies. Keep them
    // portable when the DesignIR and its localized asset directory move.
    ir.root.attributes["browser_capture_envelope"] =
        "pulp-capture:///capture.json";
    ir.root.attributes["browser_semantic_report"] =
        "pulp-capture:///" + semantic_path;
    ir.root.attributes["browser_token_report"] =
        "pulp-capture:///" + token_path;
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
    result.design_ir = std::move(ir);
    return result;
}

}  // namespace pulp::import_design
