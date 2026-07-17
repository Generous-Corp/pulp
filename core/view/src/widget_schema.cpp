#include <pulp/view/widget_schema.hpp>

#include <pulp/view/view.hpp>
#include <pulp/canvas/canvas.hpp>
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::view {

// ── Schema renderer — interprets declarative JSON widget definitions ─────────

static bool parse_schema_float_token(const std::string& token, float& out) {
    if (token.empty()) return false;
    char* end = nullptr;
    float value = std::strtof(token.c_str(), &end);
    if (end == token.c_str() || !std::isfinite(value)) return false;
    while (*end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end))) return false;
        ++end;
    }
    out = value;
    return true;
}

static int parse_schema_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::optional<canvas::Color> parse_schema_hex_color(std::string_view value) {
    if (value.empty() || value.front() != '#') return std::nullopt;
    auto pair = [](char high, char low) -> std::optional<uint8_t> {
        const int h = parse_schema_hex_digit(high);
        const int l = parse_schema_hex_digit(low);
        if (h < 0 || l < 0) return std::nullopt;
        return static_cast<uint8_t>((h << 4) | l);
    };
    if (value.size() == 7 || value.size() == 9) {
        auto r = pair(value[1], value[2]);
        auto g = pair(value[3], value[4]);
        auto b = pair(value[5], value[6]);
        auto a = value.size() == 9 ? pair(value[7], value[8]) : std::optional<uint8_t>(255);
        if (!r || !g || !b || !a) return std::nullopt;
        return canvas::Color::rgba8(*r, *g, *b, *a);
    }
    return std::nullopt;
}

void render_schema(canvas::Canvas& canvas, const std::string& json,
                   float w, float h, float value, View& view) {
    try {
        auto schema = choc::json::parse(json);
        if (!schema.isObject() || !schema.hasObjectMember("elements")) return;

        auto elements = schema["elements"];
        float cx = w * 0.5f, cy = h * 0.5f;
        float r = std::min(cx, cy) * 0.9f;

        for (uint32_t i = 0; i < elements.size(); ++i) {
            auto el = elements[i];
            auto type = el["type"].getWithDefault(std::string(""));

            // Resolve color: token name → theme color
            auto resolveColor = [&](const std::string& key, canvas::Color fallback) -> canvas::Color {
                if (!el.hasObjectMember(key)) return fallback;
                auto tok = el[key].getWithDefault(std::string(""));
                if (auto color = parse_schema_hex_color(tok))
                    return *color;
                return view.resolve_color(tok, fallback);
            };

            // Resolve dimension: percentage of widget size or absolute px
            auto resolveDim = [&](const std::string& key, float fallback) -> float {
                if (!el.hasObjectMember(key)) return fallback;
                auto s = el[key].getWithDefault(std::string(""));
                if (s.empty()) return fallback;

                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
                if (s.empty()) return fallback;

                float parsed = 0.0f;
                if (s.back() == '%') {
                    auto pct = s.substr(0, s.size() - 1);
                    return parse_schema_float_token(pct, parsed)
                        ? parsed / 100.0f * std::min(w, h) * 0.5f
                        : fallback;
                }
                return parse_schema_float_token(s, parsed) ? parsed : fallback;
            };

            // Resolve angle with optional value binding
            auto resolveAngle = [&](const std::string& key, float fallback) -> float {
                if (!el.hasObjectMember(key)) return fallback;
                auto v = el[key];
                if (v.isFloat() || v.isInt32() || v.isInt64())
                    return static_cast<float>(v.getWithDefault(0.0));
                if (v.isObject() && v.hasObjectMember("bind")) {
                    auto bind = v["bind"].getWithDefault(std::string(""));
                    if (bind == "value") {
                        auto range = v["range"];
                        float lo = range.size() > 0 ? static_cast<float>(range[0].getWithDefault(0.0)) : 0;
                        float hi = range.size() > 1 ? static_cast<float>(range[1].getWithDefault(270.0)) : 270;
                        return lo + value * (hi - lo);
                    }
                }
                return fallback;
            };

            auto lineW = static_cast<float>(el.hasObjectMember("width")
                ? el["width"].getWithDefault(3.0) : 3.0);

            if (type == "arc") {
                auto color = resolveColor("color", {100, 150, 255, 255});
                auto radius = resolveDim("radius", r);
                auto start = resolveAngle("startAngle", -135.0f);
                auto sweep = resolveAngle("sweepAngle", 270.0f);
                float startRad = start * 3.14159f / 180.0f;
                float endRad = (start + sweep) * 3.14159f / 180.0f;
                canvas.set_stroke_color(color);
                canvas.set_line_width(lineW);
                canvas.set_line_cap(canvas::LineCap::round);
                canvas.stroke_arc(cx, cy, radius, startRad, endRad);
            } else if (type == "circle") {
                auto color = resolveColor("color", {100, 150, 255, 255});
                auto radius = resolveDim("radius", r * 0.3f);
                canvas.set_fill_color(color);
                canvas.fill_circle(cx, cy, radius);
                if (el.hasObjectMember("strokeColor")) {
                    auto stroke = resolveColor("strokeColor", {60, 60, 80, 255});
                    auto stroke_width = static_cast<float>(el.hasObjectMember("strokeWidth")
                        ? el["strokeWidth"].getWithDefault(1.0) : 1.0);
                    canvas.set_stroke_color(stroke);
                    canvas.set_line_width(stroke_width);
                    canvas.stroke_circle(cx, cy, radius);
                }
            } else if (type == "line") {
                auto color = resolveColor("color", {220, 220, 220, 255});
                // Line from inner to outer at value angle
                float angle = resolveAngle("angle", -135.0f + value * 270.0f);
                float angleRad = angle * 3.14159f / 180.0f;
                float innerR = resolveDim("innerRadius", r * 0.3f);
                float outerR = resolveDim("outerRadius", r);
                canvas.set_stroke_color(color);
                canvas.set_line_width(lineW);
                canvas.set_line_cap(canvas::LineCap::round);
                canvas.stroke_line(cx + innerR * std::cos(angleRad), cy + innerR * std::sin(angleRad),
                                   cx + outerR * std::cos(angleRad), cy + outerR * std::sin(angleRad));
            } else if (type == "rect") {
                auto color = resolveColor("color", {60, 60, 80, 255});
                auto rr = resolveDim("cornerRadius", 0);
                canvas.set_fill_color(color);
                if (rr > 0) canvas.fill_rounded_rect(0, 0, w, h, rr);
                else canvas.fill_rect(0, 0, w, h);
            } else if (type == "text") {
                auto color = resolveColor("color", {200, 200, 200, 255});
                auto text = el.hasObjectMember("text") ? el["text"].getWithDefault(std::string("")) : "";
                auto size = static_cast<float>(el.hasObjectMember("fontSize")
                    ? el["fontSize"].getWithDefault(11.0) : 11.0);
                canvas.set_fill_color(color);
                canvas.set_font("Inter", size);
                canvas.set_text_align(canvas::TextAlign::center);
                canvas.fill_text(text, cx, cy);
            }
        }
    } catch (...) {
        // Invalid JSON — draw error indicator
        canvas.set_fill_color({200, 50, 50, 200});
        canvas.fill_rect(0, 0, w, h);
    }
}

}  // namespace pulp::view
