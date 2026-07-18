#include "css_color.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>

namespace pulp::view {

canvas::Color parse_bridge_css_color(std::string_view input) {
    const std::string str(input);
    canvas::Color c = canvas::Color::rgba(1.0f, 1.0f, 1.0f, 1.0f);
    if (str.empty()) return c;

    // transparent
    if (str == "transparent") return canvas::Color::rgba(0.0f, 0.0f, 0.0f, 0.0f);

    // Hex: #RGB, #RRGGBB, #RRGGBBAA
    if (str[0] == '#') {
        if (str.size() == 4) {  // #RGB → #RRGGBB
            c.r = static_cast<float>(std::stoul(std::string(2, str[1]), nullptr, 16)) / 255.0f;
            c.g = static_cast<float>(std::stoul(std::string(2, str[2]), nullptr, 16)) / 255.0f;
            c.b = static_cast<float>(std::stoul(std::string(2, str[3]), nullptr, 16)) / 255.0f;
        } else if (str.size() >= 7) {
            c.r = static_cast<float>(std::stoul(str.substr(1,2), nullptr, 16)) / 255.0f;
            c.g = static_cast<float>(std::stoul(str.substr(3,2), nullptr, 16)) / 255.0f;
            c.b = static_cast<float>(std::stoul(str.substr(5,2), nullptr, 16)) / 255.0f;
            if (str.size() >= 9)
                c.a = static_cast<float>(std::stoul(str.substr(7,2), nullptr, 16)) / 255.0f;
        }
        return c;
    }

    // rgb(r, g, b) / rgba(r, g, b, a)
    if (str.substr(0, 4) == "rgb(" || str.substr(0, 5) == "rgba(") {
        auto inner = str.substr(str.find('(') + 1);
        inner = inner.substr(0, inner.find(')'));
        float vals[4] = {0, 0, 0, 1};
        int n = 0;
        std::istringstream ss(inner);
        std::string tok;
        while (std::getline(ss, tok, ',') && n < 4) {
            while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1);
            vals[n++] = std::stof(tok);
        }
        c.r = std::clamp(vals[0] / 255.0f, 0.0f, 1.0f);
        c.g = std::clamp(vals[1] / 255.0f, 0.0f, 1.0f);
        c.b = std::clamp(vals[2] / 255.0f, 0.0f, 1.0f);
        c.a = std::clamp(vals[3], 0.0f, 1.0f);  // alpha is already 0-1 in CSS
        return c;
    }

    // hsl(h, s%, l%) / hsla(h, s%, l%, a)
    if (str.substr(0, 4) == "hsl(" || str.substr(0, 5) == "hsla(") {
        auto inner = str.substr(str.find('(') + 1);
        inner = inner.substr(0, inner.find(')'));
        float vals[4] = {0, 0, 0, 1};
        int n = 0;
        std::istringstream ss(inner);
        std::string tok;
        while (std::getline(ss, tok, ',') && n < 4) {
            while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1);
            if (tok.back() == '%') tok.pop_back();
            vals[n++] = std::stof(tok);
        }
        float h = std::fmod(vals[0], 360.0f) / 360.0f;
        float s = vals[1] / 100.0f;
        float l = vals[2] / 100.0f;
        // HSL to RGB conversion
        auto hue2rgb = [](float p, float q, float t) {
            if (t < 0) t += 1; if (t > 1) t -= 1;
            if (t < 1.0f/6) return p + (q - p) * 6 * t;
            if (t < 1.0f/2) return q;
            if (t < 2.0f/3) return p + (q - p) * (2.0f/3 - t) * 6;
            return p;
        };
        float r, g, b;
        if (s == 0) { r = g = b = l; }
        else {
            float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
            float p = 2 * l - q;
            r = hue2rgb(p, q, h + 1.0f/3);
            g = hue2rgb(p, q, h);
            b = hue2rgb(p, q, h - 1.0f/3);
        }
        c.r = std::clamp(r, 0.0f, 1.0f);
        c.g = std::clamp(g, 0.0f, 1.0f);
        c.b = std::clamp(b, 0.0f, 1.0f);
        c.a = std::clamp(vals[3], 0.0f, 1.0f);
        return c;
    }

    // Named colors (common subset)
    static const std::unordered_map<std::string, uint32_t> named = {
        {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000},
        {"green", 0x008000}, {"blue", 0x0000FF}, {"yellow", 0xFFFF00},
        {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF}, {"orange", 0xFFA500},
        {"purple", 0x800080}, {"pink", 0xFFC0CB}, {"gray", 0x808080},
        {"grey", 0x808080}, {"silver", 0xC0C0C0}, {"gold", 0xFFD700},
        {"navy", 0x000080}, {"teal", 0x008080}, {"maroon", 0x800000},
        {"olive", 0x808000}, {"lime", 0x00FF00}, {"aqua", 0x00FFFF},
        {"fuchsia", 0xFF00FF}, {"coral", 0xFF7F50}, {"salmon", 0xFA8072},
        {"tomato", 0xFF6347}, {"crimson", 0xDC143C}, {"indigo", 0x4B0082},
        {"violet", 0xEE82EE}, {"turquoise", 0x40E0D0}, {"tan", 0xD2B48C},
        {"khaki", 0xF0E68C}, {"plum", 0xDDA0DD}, {"orchid", 0xDA70D6},
        {"chocolate", 0xD2691E}, {"sienna", 0xA0522D}, {"peru", 0xCD853F},
        {"linen", 0xFAF0E6}, {"ivory", 0xFFFFF0}, {"beige", 0xF5F5DC},
        {"wheat", 0xF5DEB3}, {"snow", 0xFFFAFA}, {"azure", 0xF0FFFF},
        {"mintcream", 0xF5FFFA}, {"honeydew", 0xF0FFF0}, {"aliceblue", 0xF0F8FF},
        {"lavender", 0xE6E6FA}, {"mistyrose", 0xFFE4E1}, {"seashell", 0xFFF5EE},
        {"cornsilk", 0xFFF8DC}, {"papayawhip", 0xFFEFD5}, {"blanchedalmond", 0xFFEBCD},
        {"bisque", 0xFFE4C4}, {"moccasin", 0xFFE4B5}, {"oldlace", 0xFDF5E6},
        {"floralwhite", 0xFFFAF0}, {"ghostwhite", 0xF8F8FF}, {"whitesmoke", 0xF5F5F5},
        {"gainsboro", 0xDCDCDC}, {"lightgray", 0xD3D3D3}, {"darkgray", 0xA9A9A9},
        {"dimgray", 0x696969}, {"lightslategray", 0x778899}, {"slategray", 0x708090},
        {"darkslategray", 0x2F4F4F},
        {"lightcoral", 0xF08080}, {"indianred", 0xCD5C5C}, {"firebrick", 0xB22222},
        {"darkred", 0x8B0000}, {"orangered", 0xFF4500}, {"darkorange", 0xFF8C00},
        {"lightgreen", 0x90EE90}, {"limegreen", 0x32CD32}, {"forestgreen", 0x228B22},
        {"darkgreen", 0x006400}, {"springgreen", 0x00FF7F}, {"seagreen", 0x2E8B57},
        {"lightblue", 0xADD8E6}, {"skyblue", 0x87CEEB}, {"deepskyblue", 0x00BFFF},
        {"dodgerblue", 0x1E90FF}, {"royalblue", 0x4169E1}, {"steelblue", 0x4682B4},
        {"cornflowerblue", 0x6495ED}, {"mediumblue", 0x0000CD}, {"darkblue", 0x00008B},
        {"midnightblue", 0x191970}, {"slateblue", 0x6A5ACD}, {"mediumpurple", 0x9370DB},
        {"blueviolet", 0x8A2BE2}, {"darkviolet", 0x9400D3}, {"darkorchid", 0x9932CC},
        {"darkmagenta", 0x8B008B}, {"deeppink", 0xFF1493}, {"hotpink", 0xFF69B4},
        {"mediumvioletred", 0xC71585}, {"palevioletred", 0xDB7093},
    };
    auto it = named.find(str);
    if (it != named.end()) {
        uint32_t v = it->second;
        c = canvas::Color::rgba8((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
        return c;
    }

    return c;  // default white
}

} // namespace pulp::view
