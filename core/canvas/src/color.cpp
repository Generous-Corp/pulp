#include <pulp/canvas/canvas.hpp>
#include <cmath>
#include <cstring>

namespace pulp::canvas {

// ── HSV Conversion ──────────────────────────────────────────────────────────

Color::HSV Color::to_hsv() const {
    float rc = std::clamp(r, 0.0f, 1.0f);
    float gc = std::clamp(g, 0.0f, 1.0f);
    float bc = std::clamp(b, 0.0f, 1.0f);

    float mx = std::max({rc, gc, bc});
    float mn = std::min({rc, gc, bc});
    float d = mx - mn;

    HSV hsv;
    hsv.v = mx;
    hsv.s = (mx > 0.0f) ? (d / mx) : 0.0f;

    if (d < 1e-6f) {
        hsv.h = 0.0f;
    } else if (mx == rc) {
        hsv.h = std::fmod((gc - bc) / d + 6.0f, 6.0f) * 60.0f;
    } else if (mx == gc) {
        hsv.h = ((bc - rc) / d + 2.0f) * 60.0f;
    } else {
        hsv.h = ((rc - gc) / d + 4.0f) * 60.0f;
    }

    return hsv;
}

Color Color::from_hsv(HSV hsv, float alpha) {
    float h = std::fmod(hsv.h, 360.0f) / 60.0f;
    float s = std::clamp(hsv.s, 0.0f, 1.0f);
    float v = std::clamp(hsv.v, 0.0f, 1.0f);

    float c = v * s;
    float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    float m = v - c;

    float r1 = 0, g1 = 0, b1 = 0;
    if (h < 1)      { r1 = c; g1 = x; }
    else if (h < 2) { r1 = x; g1 = c; }
    else if (h < 3) { g1 = c; b1 = x; }
    else if (h < 4) { g1 = x; b1 = c; }
    else if (h < 5) { r1 = x; b1 = c; }
    else            { r1 = c; b1 = x; }

    return rgba(r1 + m, g1 + m, b1 + m, alpha);
}

// ── HSL Conversion ──────────────────────────────────────────────────────────

Color::HSL Color::to_hsl() const {
    float rc = std::clamp(r, 0.0f, 1.0f);
    float gc = std::clamp(g, 0.0f, 1.0f);
    float bc = std::clamp(b, 0.0f, 1.0f);

    float mx = std::max({rc, gc, bc});
    float mn = std::min({rc, gc, bc});
    float d = mx - mn;

    HSL hsl;
    hsl.l = (mx + mn) / 2.0f;

    if (d < 1e-6f) {
        hsl.h = 0.0f;
        hsl.s = 0.0f;
        return hsl;
    }

    hsl.s = (hsl.l > 0.5f) ? d / (2.0f - mx - mn) : d / (mx + mn);

    if (mx == rc)
        hsl.h = std::fmod((gc - bc) / d + 6.0f, 6.0f) * 60.0f;
    else if (mx == gc)
        hsl.h = ((bc - rc) / d + 2.0f) * 60.0f;
    else
        hsl.h = ((rc - gc) / d + 4.0f) * 60.0f;

    return hsl;
}

Color Color::from_hsl(HSL hsl, float alpha) {
    float s = std::clamp(hsl.s, 0.0f, 1.0f);
    float l = std::clamp(hsl.l, 0.0f, 1.0f);

    if (s < 1e-6f) {
        return rgba(l, l, l, alpha);
    }

    auto hue2rgb = [](float p, float q, float t) {
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
        if (t < 1.0f / 2.0f) return q;
        if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        return p;
    };

    float q = (l < 0.5f) ? l * (1.0f + s) : l + s - l * s;
    float p = 2.0f * l - q;
    float h = std::fmod(hsl.h, 360.0f) / 360.0f;

    return rgba(
        std::clamp(hue2rgb(p, q, h + 1.0f / 3.0f), 0.0f, 1.0f),
        std::clamp(hue2rgb(p, q, h), 0.0f, 1.0f),
        std::clamp(hue2rgb(p, q, h - 1.0f / 3.0f), 0.0f, 1.0f),
        alpha);
}

// ── OKLCH Conversion ────────────────────────────────────────────────────────
// OKLCH is a perceptually uniform color space (CSS Color Level 4).
// Pipeline: sRGB → linear RGB → OKLab → OKLCH

static float srgb_to_linear(float c) {
    return (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

static float linear_to_srgb(float c) {
    return (c <= 0.0031308f) ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

Color::OKLCH Color::to_oklch() const {
    // sRGB → linear
    float lr = srgb_to_linear(std::clamp(r, 0.0f, 1.0f));
    float lg = srgb_to_linear(std::clamp(g, 0.0f, 1.0f));
    float lb = srgb_to_linear(std::clamp(b, 0.0f, 1.0f));

    // Linear RGB → LMS (using Oklab M1 matrix)
    float l_ = 0.4122214708f * lr + 0.5363325363f * lg + 0.0514459929f * lb;
    float m_ = 0.2119034982f * lr + 0.6806995451f * lg + 0.1073969566f * lb;
    float s_ = 0.0883024619f * lr + 0.2817188376f * lg + 0.6299787005f * lb;

    // Cube root
    float l3 = std::cbrt(l_);
    float m3 = std::cbrt(m_);
    float s3 = std::cbrt(s_);

    // LMS → OKLab (using Oklab M2 matrix)
    float L = 0.2104542553f * l3 + 0.7936177850f * m3 - 0.0040720468f * s3;
    float A = 1.9779984951f * l3 - 2.4285922050f * m3 + 0.4505937099f * s3;
    float B = 0.0259040371f * l3 + 0.7827717662f * m3 - 0.8086757660f * s3;

    // OKLab → OKLCH
    float C = std::sqrt(A * A + B * B);
    float h = std::atan2(B, A) * (180.0f / 3.14159265358979f);
    if (h < 0.0f) h += 360.0f;

    return {L, C, h};
}

Color Color::from_oklch(OKLCH oklch, float alpha) {
    float L = std::clamp(oklch.L, 0.0f, 1.0f);
    float C = std::max(oklch.C, 0.0f);
    float h_rad = oklch.h * (3.14159265358979f / 180.0f);

    // OKLCH → OKLab
    float A = C * std::cos(h_rad);
    float B = C * std::sin(h_rad);

    // OKLab → LMS (inverse M2)
    float l3 = L + 0.3963377774f * A + 0.2158037573f * B;
    float m3 = L - 0.1055613458f * A - 0.0638541728f * B;
    float s3 = L - 0.0894841775f * A - 1.2914855480f * B;

    // Cube
    float l_ = l3 * l3 * l3;
    float m_ = m3 * m3 * m3;
    float s_ = s3 * s3 * s3;

    // LMS → linear RGB (inverse M1)
    float lr =  4.0767416621f * l_ - 3.3077115913f * m_ + 0.2309699292f * s_;
    float lg = -1.2684380046f * l_ + 2.6097574011f * m_ - 0.3413193965f * s_;
    float lb = -0.0041960863f * l_ - 0.7034186147f * m_ + 1.7076147010f * s_;

    // linear → sRGB, clamp
    return rgba(
        std::clamp(linear_to_srgb(lr), 0.0f, 1.0f),
        std::clamp(linear_to_srgb(lg), 0.0f, 1.0f),
        std::clamp(linear_to_srgb(lb), 0.0f, 1.0f),
        alpha);
}

// ── Encode / Decode ─────────────────────────────────────────────────────────

void Color::encode(uint8_t* out) const {
    std::memcpy(out, &r, sizeof(float));
    std::memcpy(out + 4, &g, sizeof(float));
    std::memcpy(out + 8, &b, sizeof(float));
    std::memcpy(out + 12, &a, sizeof(float));
}

Color Color::decode(const uint8_t* data) {
    Color c;
    std::memcpy(&c.r, data, sizeof(float));
    std::memcpy(&c.g, data + 4, sizeof(float));
    std::memcpy(&c.b, data + 8, sizeof(float));
    std::memcpy(&c.a, data + 12, sizeof(float));
    return c;
}

} // namespace pulp::canvas
