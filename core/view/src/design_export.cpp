#include <pulp/view/design_export.hpp>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace pulp::view {

// ── JSON ─────────────────────────────────────────────────────────────────

std::string DesignExport::to_json(const Theme& theme) {
    std::ostringstream ss;
    ss << "{\n";

    // Colors
    ss << "  \"colors\": {\n";
    bool first = true;
    for (const auto& [name, color] : theme.colors) {
        if (!first) ss << ",\n";
        first = false;
        ss << "    \"" << name << "\": \"#"
           << std::hex << std::setfill('0')
           << std::setw(2) << static_cast<int>(color.r)
           << std::setw(2) << static_cast<int>(color.g)
           << std::setw(2) << static_cast<int>(color.b);
        if (color.a != 255) ss << std::setw(2) << static_cast<int>(color.a);
        ss << "\"" << std::dec;
    }
    ss << "\n  },\n";

    // Dimensions
    ss << "  \"dimensions\": {\n";
    first = true;
    for (const auto& [name, value] : theme.dimensions) {
        if (!first) ss << ",\n";
        first = false;
        ss << "    \"" << name << "\": " << value;
    }
    ss << "\n  },\n";

    // Strings
    ss << "  \"strings\": {\n";
    first = true;
    for (const auto& [name, value] : theme.strings) {
        if (!first) ss << ",\n";
        first = false;
        ss << "    \"" << name << "\": \"" << value << "\"";
    }
    ss << "\n  }\n";

    ss << "}\n";
    return ss.str();
}

// ── CSS ──────────────────────────────────────────────────────────────────

std::string DesignExport::to_css(const Theme& theme, const std::string& prefix) {
    std::ostringstream ss;
    ss << ":root {\n";

    for (const auto& [name, color] : theme.colors) {
        ss << "  --" << prefix << "-" << name << ": #"
           << std::hex << std::setfill('0')
           << std::setw(2) << static_cast<int>(color.r)
           << std::setw(2) << static_cast<int>(color.g)
           << std::setw(2) << static_cast<int>(color.b);
        if (color.a != 255) ss << std::setw(2) << static_cast<int>(color.a);
        ss << ";\n" << std::dec;
    }

    for (const auto& [name, value] : theme.dimensions) {
        ss << "  --" << prefix << "-" << name << ": " << value << "px;\n";
    }

    for (const auto& [name, value] : theme.strings) {
        ss << "  --" << prefix << "-" << name << ": \"" << value << "\";\n";
    }

    ss << "}\n";
    return ss.str();
}

// ── C++ header ───────────────────────────────────────────────────────────

std::string DesignExport::to_cpp_header(const Theme& theme, const std::string& ns) {
    std::ostringstream ss;
    ss << "#pragma once\n\n";
    ss << "#include <cstdint>\n\n";
    ss << "namespace " << ns << " {\n\n";

    ss << "// Colors (ARGB)\n";
    for (const auto& [name, color] : theme.colors) {
        uint32_t argb = (static_cast<uint32_t>(color.a) << 24) |
                        (static_cast<uint32_t>(color.r) << 16) |
                        (static_cast<uint32_t>(color.g) << 8) |
                        static_cast<uint32_t>(color.b);
        // Convert name to kCamelCase
        std::string const_name = "k";
        bool capitalize_next = true;
        for (char c : name) {
            if (c == '_' || c == '-') { capitalize_next = true; continue; }
            const_name += capitalize_next ? static_cast<char>(std::toupper(c)) : c;
            capitalize_next = false;
        }
        ss << "constexpr uint32_t " << const_name << " = 0x"
           << std::hex << std::setfill('0') << std::setw(8) << argb
           << ";\n" << std::dec;
    }

    ss << "\n// Dimensions\n";
    for (const auto& [name, value] : theme.dimensions) {
        std::string const_name = "k";
        bool capitalize_next = true;
        for (char c : name) {
            if (c == '_' || c == '-') { capitalize_next = true; continue; }
            const_name += capitalize_next ? static_cast<char>(std::toupper(c)) : c;
            capitalize_next = false;
        }
        ss << "constexpr float " << const_name << " = " << value << "f;\n";
    }

    ss << "\n} // namespace " << ns << "\n";
    return ss.str();
}

// ── OKLCH CSS ────────────────────────────────────────────────────────────

static void rgb_to_oklch(uint8_t r, uint8_t g, uint8_t b, float& L, float& C, float& h) {
    // Linearize sRGB
    auto linear = [](float v) -> float {
        v /= 255.0f;
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    float lr = linear(static_cast<float>(r));
    float lg = linear(static_cast<float>(g));
    float lb = linear(static_cast<float>(b));

    // sRGB to OKLab (simplified)
    float l_ = 0.4122214708f * lr + 0.5363325363f * lg + 0.0514459929f * lb;
    float m_ = 0.2119034982f * lr + 0.6806995451f * lg + 0.1073969566f * lb;
    float s_ = 0.0883024619f * lr + 0.2817188376f * lg + 0.6299787005f * lb;

    l_ = std::cbrt(l_); m_ = std::cbrt(m_); s_ = std::cbrt(s_);

    float lab_L = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
    float lab_a = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
    float lab_b = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;

    L = lab_L;
    C = std::sqrt(lab_a * lab_a + lab_b * lab_b);
    h = std::atan2(lab_b, lab_a) * 180.0f / 3.14159265f;
    if (h < 0) h += 360.0f;
}

std::string DesignExport::to_oklch_css(const Theme& theme, const std::string& prefix) {
    std::ostringstream ss;
    ss << ":root {\n";
    ss << std::fixed << std::setprecision(3);

    for (const auto& [name, color] : theme.colors) {
        float L, C, h;
        rgb_to_oklch(color.r, color.g, color.b, L, C, h);
        ss << "  --" << prefix << "-" << name << ": oklch("
           << L << " " << C << " " << h;
        if (color.a != 255) ss << " / " << (color.a / 255.0f);
        ss << ");\n";
    }

    ss << "}\n";
    return ss.str();
}

// ── WGSL ─────────────────────────────────────────────────────────────────

std::string DesignExport::to_wgsl_uniforms(const Theme& theme, const std::string& name) {
    std::ostringstream ss;
    ss << "struct " << name << " {\n";

    for (const auto& [cname, color] : theme.colors) {
        std::string field = cname;
        for (auto& c : field) if (c == '-') c = '_';
        ss << "  " << field << ": vec4<f32>,  // #"
           << std::hex << std::setfill('0')
           << std::setw(2) << static_cast<int>(color.r)
           << std::setw(2) << static_cast<int>(color.g)
           << std::setw(2) << static_cast<int>(color.b)
           << "\n" << std::dec;
    }

    for (const auto& [dname, value] : theme.dimensions) {
        std::string field = dname;
        for (auto& c : field) if (c == '-') c = '_';
        ss << "  " << field << ": f32,  // " << value << "\n";
    }

    ss << "};\n";
    return ss.str();
}

} // namespace pulp::view
