#include "tools/import-design/annotated_capture.hpp"

#include <cctype>
#include <cstdio>
#include <optional>
#include <sstream>
#include <unordered_map>

#include <choc/text/choc_JSON.h>

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
    return (m.isInt() || m.isFloat()) ? static_cast<float>(m.getWithDefault(0.0))
                                      : dflt;
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
        e.target_frame = static_cast<int>(num(ev, "target_frame",
                                              static_cast<float>(e.target_frame)));

        out.elements.push_back(std::move(e));
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
          << "    // runs unchanged embedded in JUCE / iPlug2 / native.\n"
          << "    route_changes_to_host_params(true);\n";
    }
    o << "}\n\n"
      << "// Embedded SVG accessor placeholder — the capture's base64 SVG is emitted\n"
      << "// into detail::" << snk << "_svg_b64() by the lane (see " << svg_b64_symbol << ").\n"
      << "}  // namespace pulp::view\n";
    return o.str();
}

}  // namespace pulp::import_design
