#include "tools/import-design/annotated_capture.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <regex>
#include <string_view>
#include <unordered_set>
#include <sstream>
#include <unordered_map>

#include <choc/text/choc_JSON.h>
#include <pulp/runtime/crypto.hpp>

namespace pulp::import_design {

using view::DesignFrameElement;

bool AnnotatedCaptureManifest::has_param_bindings() const {
    for (const auto& e : elements)
        if (!e.param_key.empty()) return true;
    return false;
}

namespace {

std::optional<DesignFrameElement::Kind> kind_from_string(const std::string& s) {
    using K = DesignFrameElement::Kind;
    static const std::unordered_map<std::string, K> kMap = {
        {"knob", K::knob}, {"fader", K::fader}, {"toggle", K::toggle},
        {"text_field", K::text_field}, {"dropdown", K::dropdown},
        {"tab_group", K::tab_group}, {"stepper", K::stepper},
        {"momentary", K::momentary}, {"swap", K::swap}, {"action", K::action},
        {"value_label", K::value_label}, {"xy_pad", K::xy_pad},
        {"custom", K::custom},
    };
    const auto it = kMap.find(s);
    if (it == kMap.end()) return std::nullopt;
    return it->second;
}

const char* kind_to_enum(DesignFrameElement::Kind k) {
    using K = DesignFrameElement::Kind;
    switch (k) {
        case K::knob: return "knob";
        case K::fader: return "fader";
        case K::toggle: return "toggle";
        case K::text_field: return "text_field";
        case K::dropdown: return "dropdown";
        case K::tab_group: return "tab_group";
        case K::stepper: return "stepper";
        case K::momentary: return "momentary";
        case K::swap: return "swap";
        case K::action: return "action";
        case K::value_label: return "value_label";
        case K::xy_pad: return "xy_pad";
        case K::custom: return "custom";
    }
    return "knob";
}

float num(const choc::value::ValueView& v, const char* key, float dflt) {
    if (!v.isObject() || !v.hasObjectMember(key)) return dflt;
    const auto m = v[key];
    if (!m.isInt() && !m.isFloat())
        return dflt;
    const auto value = static_cast<float>(m.getWithDefault(0.0));
    return std::isfinite(value) ? value : std::numeric_limits<float>::quiet_NaN();
}

bool contains_non_finite_number(const choc::value::ValueView& value) {
    if (value.isInt() || value.isFloat())
        return !std::isfinite(value.getWithDefault(0.0));
    if (value.isArray()) {
        for (uint32_t i = 0; i < value.size(); ++i)
            if (contains_non_finite_number(value[i]))
                return true;
    } else if (value.isObject()) {
        for (uint32_t i = 0; i < value.size(); ++i)
            if (contains_non_finite_number(value.getObjectMemberAt(i).value))
                return true;
    }
    return false;
}

std::string str(const choc::value::ValueView& v, const char* key) {
    if (!v.isObject() || !v.hasObjectMember(key)) return {};
    const auto m = v[key];
    return m.isString() ? std::string(m.getString()) : std::string{};
}

// C++ string literal escaping for the generated source.
std::string esc(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string fnum(float f) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(f));
    std::string s(buf);
    // A "%g" integer print (e.g. "50") needs a decimal point before the `f`
    // suffix — "50f" is an invalid literal, "50.f" is valid.
    if (s.find_first_of(".eEnN") == std::string::npos) s += '.';
    return s + "f";
}

}  // namespace

bool parse_annotated_manifest(const std::string& json,
                              AnnotatedCaptureManifest& out, std::string& error) {
    error.clear();
    out = {};
    choc::value::Value root;
    try {
        root = choc::json::parse(json);
    } catch (const std::exception& e) {
        error = std::string("invalid manifest JSON: ") + e.what();
        return false;
    }
    if (!root.isObject()) {
        error = "manifest root must be a JSON object";
        return false;
    }
    out.name = str(root, "name");
    out.class_name = str(root, "class");

    if (!root.hasObjectMember("elements") || !root["elements"].isArray()) {
        error = "manifest requires an \"elements\" array";
        return false;
    }
    const auto elems = root["elements"];
    for (uint32_t i = 0; i < elems.size(); ++i) {
        const auto ev = elems[i];
        if (!ev.isObject()) {
            error = "element " + std::to_string(i) + " is not an object";
            return false;
        }
        const std::string kind_s = str(ev, "kind");
        const auto kind = kind_from_string(kind_s);
        if (!kind) {
            error = "element " + std::to_string(i) + " has missing/unknown kind '" +
                    kind_s + "'";
            return false;
        }
        DesignFrameElement e;
        e.kind = *kind;
        e.source_node_id = str(ev, "selector");
        e.param_key = str(ev, "param_key");
        e.needle_d = str(ev, "needle");
        e.action = str(ev, "action");
        e.text = str(ev, "text");
        e.placeholder = str(ev, "placeholder");
        e.bg_color = str(ev, "bg_color");
        e.factory_id = str(ev, "factory_id");
        e.custom_props = str(ev, "custom_props");

        // Geometry sub-object (all optional; keep DesignFrameElement defaults).
        if (ev.hasObjectMember("geometry")) {
            const auto g = ev["geometry"];
            e.cx = num(g, "cx", e.cx);
            e.cy = num(g, "cy", e.cy);
            e.hit_radius = num(g, "hit_radius", e.hit_radius);
            e.x = num(g, "x", e.x);
            e.y = num(g, "y", e.y);
            e.w = num(g, "w", e.w);
            e.h = num(g, "h", e.h);
            e.value = num(g, "value", e.value);
            e.value_y = num(g, "value_y", e.value_y);
        }

        if (ev.hasObjectMember("options") && ev["options"].isArray()) {
            const auto opts = ev["options"];
            for (uint32_t j = 0; j < opts.size(); ++j)
                if (opts[j].isString()) e.options.emplace_back(opts[j].getString());
        }
        e.selected_index = static_cast<int>(num(ev, "selected_index",
                                                static_cast<float>(e.selected_index)));
        e.note = static_cast<int>(num(ev, "note", static_cast<float>(e.note)));
        e.view_group = static_cast<int>(num(ev, "view_group",
                                            static_cast<float>(e.view_group)));
        // `target_frame` round-trips through this lane but cannot resolve here:
        // the sidecar manifest carries a single SVG and a single element list,
        // with no surface for declaring additional frames, so nothing in this
        // lane calls DesignFrameView::add_frame() and the generated view owns
        // exactly one frame. A `swap` element therefore hit-tests but never
        // changes frame — set_active_frame() ignores any index outside the
        // installed frame set. Multi-frame swap links come from the Figma lane,
        // which lowers a faithful_svg node's alternate_frames to add_frame().
        e.target_frame = static_cast<int>(num(ev, "target_frame",
                                              static_cast<float>(e.target_frame)));

        out.elements.push_back(std::move(e));
    }
    return true;
}

bool validate_annotated_capture(const std::string& svg, const std::string& sidecar_json,
                                std::string& error) {
    error.clear();
    choc::value::Value root;
    try {
        root = choc::json::parse(sidecar_json);
    } catch (const std::exception& e) {
        error = std::string("invalid sidecar JSON: ") + e.what();
        return false;
    }
    if (!root.isObject()) {
        error = "sidecar root must be an object";
        return false;
    }
    if (contains_non_finite_number(root)) {
        error = "sidecar contains a non-finite numeric value";
        return false;
    }
    if (!root.hasObjectMember("provenance") || !root["provenance"].isObject()) {
        error = "sidecar requires provenance object";
        return false;
    }
    const auto provenance = root["provenance"];
    const auto source = str(provenance, "source");
    if (source.empty()) {
        error = "provenance.source is required";
        return false;
    }
    const auto hash = str(root, "svg_sha256");
    static const std::regex hex64("^[0-9a-fA-F]{64}$");
    if (!std::regex_match(hash, hex64)) {
        error = "svg_sha256 must be a 64-character hex digest";
        return false;
    }
    if (runtime::sha256_hex(svg) != hash) {
        error = "svg_sha256 does not match SVG bytes";
        return false;
    }
    // A matching digest authenticates the bytes, but it does not establish that
    // they are an SVG document. Keep this lane fail-closed before any future
    // generator consumes the capture: require one SVG root envelope even when
    // the sidecar declares no controls. This intentionally remains a bounded
    // structural check; full XML/SVG semantics belong to the rendering backend.
    static const std::regex svg_root_re(R"(<svg(?:\s|>))");
    static const std::regex svg_close_re(R"(</svg\s*>)");
    if (!std::regex_search(svg, svg_root_re) || !std::regex_search(svg, svg_close_re)) {
        error = "SVG bytes must contain an <svg> root element";
        return false;
    }
    if (!root.hasObjectMember("canvas") || !root["canvas"].isObject()) {
        error = "sidecar requires canvas object";
        return false;
    }
    const auto canvas = root["canvas"];
    for (const char* key : {"width", "height"}) {
        if (!canvas.hasObjectMember(key) || !(canvas[key].isInt() || canvas[key].isFloat())) {
            error = std::string("canvas ") + key + " must be numeric";
            return false;
        }
    }
    const float cw = num(canvas, "width", 0), ch = num(canvas, "height", 0);
    if (!(std::isfinite(cw) && std::isfinite(ch) && cw > 0 && ch > 0)) {
        error = "canvas width and height must be positive";
        return false;
    }

    // Remove non-markup regions before looking for start tags. Otherwise a
    // nested `<rect id="x">` inside a comment, CDATA block, or script/style
    // string could be mistaken for a real SVG element. Unclosed regions consume
    // the rest of the source so malformed envelopes fail closed.
    std::string svg_markup;
    const auto starts_ci = [&](size_t pos, std::string_view token) {
        if (pos + token.size() > svg.size()) return false;
        for (size_t j = 0; j < token.size(); ++j)
            if (std::tolower(static_cast<unsigned char>(svg[pos + j])) !=
                std::tolower(static_cast<unsigned char>(token[j])))
                return false;
        return true;
    };
    for (size_t i = 0; i < svg.size();) {
        if (svg.compare(i, 4, "<!--") == 0) {
            const auto end = svg.find("-->", i + 4);
            if (end == std::string::npos) break;
            i = end + 3;
        } else if (svg.compare(i, 9, "<![CDATA[") == 0) {
            const auto end = svg.find("]]>", i + 9);
            if (end == std::string::npos) break;
            i = end + 3;
        } else if (starts_ci(i, "<script") || starts_ci(i, "<style")) {
            const auto name_end = svg.find('>', i + 1);
            if (name_end == std::string::npos) break;
            const auto name = starts_ci(i, "<script") ? "script" : "style";
            const auto close = svg.find(std::string("</") + name, name_end + 1);
            if (close == std::string::npos) break;
            const auto close_end = svg.find('>', close + 2 + std::strlen(name));
            if (close_end == std::string::npos) break;
            i = close_end + 1;
        } else if (svg.compare(i, 2, "<?") == 0 || svg.compare(i, 2, "<!") == 0) {
            const auto end = svg.find('>', i + 2);
            if (end == std::string::npos) break;
            i = end + 1;
        } else {
            svg_markup.push_back(svg[i++]);
        }
    }

    std::unordered_set<std::string> svg_ids;
    // Restrict attribute matching to actual start tags. A word-boundary match
    // over the whole source would accept `data-id`, `aria-id`, or comments as
    // if they were real SVG id attributes.
    const std::regex tag_re(R"(<\s*[A-Za-z][^<>]*>)");
    const std::regex id_re(R"((?:^|\s)id\s*=\s*["']([^"']+)["'])");
    for (std::sregex_iterator tag(svg_markup.begin(), svg_markup.end(), tag_re), end; tag != end;
         ++tag) {
        const auto tag_text = tag->str();
        for (std::sregex_iterator id(tag_text.begin(), tag_text.end(), id_re), id_end; id != id_end;
             ++id)
            if (!svg_ids.insert((*id)[1].str()).second) {
                error = "SVG contains duplicate id '" + (*id)[1].str() + "'";
                return false;
            }
    }

    AnnotatedCaptureManifest manifest;
    if (!root.hasObjectMember("elements") || !root["elements"].isArray()) {
        error = "sidecar requires elements array";
        return false;
    }
    const auto elements = root["elements"];
    for (uint32_t i = 0; i < elements.size(); ++i) {
        const auto element = elements[i];
        if (!element.isObject())
            continue;
        const auto check_numeric = [&](const choc::value::ValueView& object, const char* key) {
            return !object.hasObjectMember(key) || object[key].isInt() || object[key].isFloat();
        };
        for (const char* key : {"selected_index", "note", "view_group", "target_frame"})
            if (!check_numeric(element, key)) {
                error = "element " + std::to_string(i) + " field '" + key + "' must be numeric";
                return false;
            }
        if (!element.hasObjectMember("geometry") || !element["geometry"].isObject()) {
            error = "element " + std::to_string(i) + " requires a geometry object";
            return false;
        }
        const auto geometry = element["geometry"];
        for (const char* key : {"cx", "cy", "hit_radius", "x", "y", "w", "h", "value", "value_y"})
            if (!check_numeric(geometry, key)) {
                error = "element " + std::to_string(i) + " geometry field '" + key +
                        "' must be numeric";
                return false;
            }
        const auto kind = element.hasObjectMember("kind") && element["kind"].isString()
                              ? std::string(element["kind"].getString())
                              : std::string{};
        const bool circular = kind == "knob";
        const bool xy = kind == "xy_pad";
        const auto require_geometry = [&](std::initializer_list<const char*> keys) {
            for (const auto* key : keys)
                if (!geometry.hasObjectMember(key)) {
                    error = "element " + std::to_string(i) + " geometry requires '" + key +
                            "' for kind '" + kind + "'";
                    return false;
                }
            return true;
        };
        if (circular) {
            if (!require_geometry({"cx", "cy", "hit_radius"}))
                return false;
        } else if (xy) {
            if (!require_geometry({"x", "y", "w", "h", "cx", "cy"}))
                return false;
        } else {
            if (!require_geometry({"x", "y", "w", "h"}))
                return false;
        }
    }
    if (!parse_annotated_manifest(sidecar_json, manifest, error))
        return false;
    std::unordered_set<std::string> selectors;
    for (size_t i = 0; i < manifest.elements.size(); ++i) {
        const auto& e = manifest.elements[i];
        if (!std::isfinite(e.cx) || !std::isfinite(e.cy) || !std::isfinite(e.hit_radius) ||
            !std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.w) ||
            !std::isfinite(e.h) || !std::isfinite(e.value) || !std::isfinite(e.value_y)) {
            error = "element " + std::to_string(i) + " contains a non-finite numeric value";
            return false;
        }
        if (e.source_node_id.size() < 2 || e.source_node_id[0] != '#') {
            error = "element " + std::to_string(i) + " selector must be an SVG #id";
            return false;
        }
        const auto id = e.source_node_id.substr(1);
        if (!selectors.insert(id).second) {
            error = "duplicate control selector '" + e.source_node_id + "'";
            return false;
        }
        if (!svg_ids.count(id)) {
            error = "control selector '" + e.source_node_id + "' is absent from SVG";
            return false;
        }
        if (e.w < 0 || e.h < 0 || e.x < 0 || e.y < 0 || e.x + e.w > cw || e.y + e.h > ch) {
            error = "element " + std::to_string(i) + " geometry is outside canvas";
            return false;
        }
        if ((e.kind == DesignFrameElement::Kind::knob ||
             e.kind == DesignFrameElement::Kind::xy_pad) &&
            (e.cx < 0 || e.cy < 0 || e.cx > cw || e.cy > ch || e.hit_radius < 0 ||
             e.cx + e.hit_radius > cw || e.cy + e.hit_radius > ch || e.cx - e.hit_radius < 0 ||
             e.cy - e.hit_radius < 0)) {
            error = "element " + std::to_string(i) + " control center is outside canvas";
            return false;
        }
    }
    return true;
}

std::string snake_case(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (std::isupper(static_cast<unsigned char>(c))) {
            if (i > 0) out += '_';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out;
}

std::string generate_view_header(const AnnotatedCaptureManifest& m) {
    const std::string cls = m.class_name.empty() ? "AnnotatedCaptureView"
                                                  : m.class_name;
    std::ostringstream o;
    o << "#pragma once\n\n"
      << "#include <pulp/view/design_frame_view.hpp>\n\n"
      << "namespace pulp::view {\n\n"
      << "// " << (m.name.empty() ? cls : m.name)
      << " — generated by tools/import-design annotated-capture lane from a bare\n"
      << "// SVG capture + sidecar manifest. Renders the SVG 1:1 via DesignFrameView\n"
      << "// (SkSVGDOM) with a typed interactive-element overlay. Regenerate from the\n"
      << "// manifest; do not hand-edit.\n"
      << "class " << cls << " : public DesignFrameView {\npublic:\n"
      << "    " << cls << "();\n};\n\n"
      << "}  // namespace pulp::view\n";
    return o.str();
}

std::string generate_view_source(const AnnotatedCaptureManifest& m,
                                 const std::string& svg_b64_symbol) {
    const std::string cls = m.class_name.empty() ? "AnnotatedCaptureView"
                                                  : m.class_name;
    const std::string snk = snake_case(cls);
    std::ostringstream o;
    o << "#include <pulp/view/" << snk << ".hpp>\n"
      << "#include <pulp/runtime/base64.hpp>\n\n"
      << "#include <string>\n#include <vector>\n\n"
      << "namespace pulp::view {\n\n"
      << "// Embedded SVG accessor (emitted alongside, mirroring the Figma path's\n"
      << "// <snake>_svg.cpp).\n"
      << "namespace detail { const char* " << snk << "_svg_b64(); }\n\n"
      << "namespace {\n\n"
      << "std::string decode_" << snk << "_svg() {\n"
      << "    if (auto bytes = runtime::base64_decode(detail::" << snk << "_svg_b64()))\n"
      << "        return std::string(bytes->begin(), bytes->end());\n"
      << "    return {};\n}\n\n"
      << "std::vector<DesignFrameElement> build_" << snk << "_elements() {\n"
      << "    std::vector<DesignFrameElement> els;\n"
      << "    els.reserve(" << m.elements.size() << ");\n";

    for (const auto& e : m.elements) {
        o << "    {\n"
          << "        DesignFrameElement e;\n"
          << "        e.kind = DesignFrameElement::Kind::" << kind_to_enum(e.kind) << ";\n";
        auto emit_f = [&](const char* field, float v, float dflt) {
            if (v != dflt) o << "        e." << field << " = " << fnum(v) << ";\n";
        };
        auto emit_i = [&](const char* field, int v, int dflt) {
            if (v != dflt) o << "        e." << field << " = " << v << ";\n";
        };
        auto emit_s = [&](const char* field, const std::string& v) {
            if (!v.empty()) o << "        e." << field << " = \"" << esc(v) << "\";\n";
        };
        emit_f("cx", e.cx, 0.0f);
        emit_f("cy", e.cy, 0.0f);
        emit_f("hit_radius", e.hit_radius, 0.0f);
        emit_f("x", e.x, 0.0f);
        emit_f("y", e.y, 0.0f);
        emit_f("w", e.w, 0.0f);
        emit_f("h", e.h, 0.0f);
        emit_f("value", e.value, 0.5f);
        emit_f("value_y", e.value_y, 0.5f);
        emit_s("needle_d", e.needle_d);
        emit_s("action", e.action);
        emit_s("text", e.text);
        emit_s("placeholder", e.placeholder);
        emit_s("bg_color", e.bg_color);
        emit_s("factory_id", e.factory_id);
        emit_s("custom_props", e.custom_props);
        emit_s("param_key", e.param_key);
        emit_s("source_node_id", e.source_node_id);
        emit_i("selected_index", e.selected_index, 0);
        emit_i("note", e.note, -1);
        emit_i("view_group", e.view_group, -1);
        // Emitted for schema parity; inert in this lane's single-frame view.
        emit_i("target_frame", e.target_frame, -1);
        if (!e.options.empty()) {
            o << "        e.options = {";
            for (size_t j = 0; j < e.options.size(); ++j)
                o << (j ? ", " : "") << "\"" << esc(e.options[j]) << "\"";
            o << "};\n";
        }
        o << "        els.push_back(std::move(e));\n    }\n";
    }

    o << "    return els;\n}\n\n"
      << "}  // namespace\n\n"
      << cls << "::" << cls << "()\n"
      << "    : DesignFrameView(decode_" << snk << "_svg(), build_" << snk << "_elements()) {\n";
    if (m.has_param_bindings()) {
        o << "    // The manifest declares host-parameter bindings — self-wire user\n"
          << "    // gestures to the framework-agnostic HostParamSurface so this view\n"
          << "    // runs unchanged in any host, whatever its parameter system.\n"
          << "    route_changes_to_host_params(true);\n";
    }
    o << "}\n\n"
      << "// Embedded SVG accessor placeholder — the capture's base64 SVG is emitted\n"
      << "// into detail::" << snk << "_svg_b64() by the lane (see " << svg_b64_symbol << ").\n"
      << "}  // namespace pulp::view\n";
    return o.str();
}

}  // namespace pulp::import_design
