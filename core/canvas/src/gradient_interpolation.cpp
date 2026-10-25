// Reference colour interpolation for CSS gradients (CSS Color 4 §12).
//
// Conversion matrices and transfer functions are the ones CSS Color 4 publishes
// in its sample code, so a curve sampled here agrees with any implementation
// that follows the spec. Arithmetic is double precision: the spaces are reached
// through two or three matrix hops, and float error would surface as a hue
// wobble on near-grey stops.

#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/gradient_interpolation.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pulp::canvas {

namespace {

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<Vec3, 3>;

constexpr double kPi = 3.14159265358979323846;

Vec3 mul(const Mat3& m, const Vec3& v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
            m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

// ── Transfer functions (sign-preserving, so out-of-range values survive) ──

double srgb_to_linear(double c) {
    const double a = std::fabs(c);
    const double v = a <= 0.04045 ? a / 12.92 : std::pow((a + 0.055) / 1.055, 2.4);
    return std::copysign(v, c);
}

double srgb_from_linear(double c) {
    const double a = std::fabs(c);
    const double v = a <= 0.0031308 ? a * 12.92 : 1.055 * std::pow(a, 1.0 / 2.4) - 0.055;
    return std::copysign(v, c);
}

double a98_to_linear(double c) {
    return std::copysign(std::pow(std::fabs(c), 563.0 / 256.0), c);
}
double a98_from_linear(double c) {
    return std::copysign(std::pow(std::fabs(c), 256.0 / 563.0), c);
}

double prophoto_to_linear(double c) {
    const double a = std::fabs(c);
    return a <= 16.0 / 512.0 ? c / 16.0 : std::copysign(std::pow(a, 1.8), c);
}

double prophoto_from_linear(double c) {
    const double a = std::fabs(c);
    return a >= 1.0 / 512.0 ? std::copysign(std::pow(a, 1.0 / 1.8), c) : 16.0 * c;
}

constexpr double kRec2020Alpha = 1.09929682680944;
constexpr double kRec2020Beta = 0.018053968510807;

double rec2020_to_linear(double c) {
    const double a = std::fabs(c);
    const double v = a < kRec2020Beta * 4.5
                         ? a / 4.5
                         : std::pow((a + kRec2020Alpha - 1.0) / kRec2020Alpha, 1.0 / 0.45);
    return std::copysign(v, c);
}

double rec2020_from_linear(double c) {
    const double a = std::fabs(c);
    const double v =
        a < kRec2020Beta ? 4.5 * a : kRec2020Alpha * std::pow(a, 0.45) - (kRec2020Alpha - 1.0);
    return std::copysign(v, c);
}

// ── Matrices (rows map the source space's linear RGB onto XYZ) ──

constexpr Mat3 kLinSrgbToXyz65{{{0.41239079926595934, 0.357584339383878, 0.1804807884018343},
                                {0.21263900587151027, 0.715168678767756, 0.07219231536073371},
                                {0.01933081871559182, 0.11919477979462598, 0.9505321522496607}}};
constexpr Mat3 kXyz65ToLinSrgb{{{3.2409699419045226, -1.537383177570094, -0.4986107602930034},
                                {-0.9692436362808796, 1.8759675015077202, 0.04155505740717559},
                                {0.05563007969699366, -0.20397695888897652, 1.0569715142428786}}};
constexpr Mat3 kLinP3ToXyz65{{{0.4865709486482162, 0.26566769316909306, 0.1982172852343625},
                              {0.2289745640697488, 0.6917385218365064, 0.079286914093745},
                              {0.0, 0.04511338185890264, 1.043944368900976}}};
constexpr Mat3 kXyz65ToLinP3{{{2.493496911941425, -0.9313836179191239, -0.40271078445071684},
                              {-0.8294889695615747, 1.7626640603183463, 0.023624685841943577},
                              {0.03584583024378447, -0.07617238926804182, 0.9568845240076872}}};
constexpr Mat3 kLinA98ToXyz65{{{0.5766690429101305, 0.1855582379065463, 0.1882286462349947},
                               {0.29734497525053605, 0.6273635662554661, 0.0752914584939979},
                               {0.02703136138641234, 0.07068885253582723, 0.9913375368376388}}};
constexpr Mat3 kXyz65ToLinA98{{{2.0415879038107465, -0.5650069742788596, -0.34473135077832956},
                               {-0.9692436362808795, 1.8759675015077202, 0.04155505740717557},
                               {0.013444280632031142, -0.11836239223101838, 1.0151749943912054}}};
constexpr Mat3 kLin2020ToXyz65{{{0.6369580483012914, 0.14461690358620832, 0.1688809751641721},
                                {0.2627002120112671, 0.6779980715188708, 0.05930171646986196},
                                {0.0, 0.028072693049087428, 1.060985057710791}}};
constexpr Mat3 kXyz65ToLin2020{{{1.7166511879712674, -0.35567078377639233, -0.25336628137365974},
                                {-0.6666843518324892, 1.6164812366349395, 0.01576854581391113},
                                {0.017639857445310783, -0.042770613257808524, 0.9421031212354738}}};
constexpr Mat3 kLinProphotoToXyz50{{{0.7977666449006423, 0.13518129740053308, 0.0313477341283922},
                                    {0.2880748288194013, 0.711835234241873, 0.00008993693872564},
                                    {0.0, 0.0, 0.8251046025104602}}};
constexpr Mat3 kXyz50ToLinProphoto{
    {{1.3457868816471583, -0.25557208737979464, -0.05110186497554526},
     {-0.5446307051249019, 1.5082477428451468, 0.02052744743642139},
     {0.0, 0.0, 1.2119675456389452}}};
// Bradford chromatic adaptation between the D65 and D50 whites.
constexpr Mat3 kXyz65ToXyz50{{{1.0479298208405488, 0.022946793341019088, -0.05019222954313557},
                              {0.029627815688159344, 0.990434484573249, -0.01707382502938514},
                              {-0.009243058152591178, 0.015055144896577895, 0.7518742899580008}}};
constexpr Mat3 kXyz50ToXyz65{{{0.9554734527042182, -0.023098536874261423, 0.0632593086610217},
                              {-0.028369706963208136, 1.0099954580106629, 0.021041398966943008},
                              {0.012314001688319899, -0.020507696433477912, 1.3303659366080753}}};

constexpr Vec3 kD50White{0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585};

Vec3 each(const Vec3& v, double (*fn)(double)) {
    return {fn(v[0]), fn(v[1]), fn(v[2])};
}

// ── Lab / OKLab ──

Vec3 xyz50_to_lab(const Vec3& xyz) {
    constexpr double eps = 216.0 / 24389.0, kappa = 24389.0 / 27.0;
    const auto f = [&](double v) { return v > eps ? std::cbrt(v) : (kappa * v + 16.0) / 116.0; };
    const double fx = f(xyz[0] / kD50White[0]);
    const double fy = f(xyz[1] / kD50White[1]);
    const double fz = f(xyz[2] / kD50White[2]);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

Vec3 lab_to_xyz50(const Vec3& lab) {
    constexpr double eps = 216.0 / 24389.0, kappa = 24389.0 / 27.0;
    const double fy = (lab[0] + 16.0) / 116.0;
    const double fx = lab[1] / 500.0 + fy;
    const double fz = fy - lab[2] / 200.0;
    const double x = fx * fx * fx > eps ? fx * fx * fx : (116.0 * fx - 16.0) / kappa;
    const double y = lab[0] > kappa * eps ? fy * fy * fy : lab[0] / kappa;
    const double z = fz * fz * fz > eps ? fz * fz * fz : (116.0 * fz - 16.0) / kappa;
    return {x * kD50White[0], y * kD50White[1], z * kD50White[2]};
}

Vec3 lin_srgb_to_oklab(const Vec3& c) {
    const double l = std::cbrt(0.4122214708 * c[0] + 0.5363325363 * c[1] + 0.0514459929 * c[2]);
    const double m = std::cbrt(0.2119034982 * c[0] + 0.6806995451 * c[1] + 0.1073969566 * c[2]);
    const double s = std::cbrt(0.0883024619 * c[0] + 0.2817188376 * c[1] + 0.6299787005 * c[2]);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

Vec3 oklab_to_lin_srgb(const Vec3& lab) {
    const double l1 = lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2];
    const double m1 = lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2];
    const double s1 = lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2];
    const double l = l1 * l1 * l1, m = m1 * m1 * m1, s = s1 * s1 * s1;
    return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
}

double normalize_hue(double h) {
    h = std::fmod(h, 360.0);
    return h < 0.0 ? h + 360.0 : h;
}

// Rectangular (L, a, b) -> polar (L, C, h); h is NaN when the colour has no
// chroma, which is CSS's "powerless" hue.
Vec3 to_polar(const Vec3& lab, double powerless_chroma) {
    const double c = std::hypot(lab[1], lab[2]);
    const double h = c <= powerless_chroma
                         ? std::nan("")
                         : normalize_hue(std::atan2(lab[2], lab[1]) * 180.0 / kPi);
    return {lab[0], c, h};
}

Vec3 from_polar(const Vec3& lch) {
    const double h = std::isnan(lch[2]) ? 0.0 : lch[2] * kPi / 180.0;
    return {lch[0], lch[1] * std::cos(h), lch[1] * std::sin(h)};
}

// ── HSL / HWB, over gamma-encoded sRGB ──

Vec3 srgb_to_hsl(const Vec3& rgb) {
    const double mx = std::max({rgb[0], rgb[1], rgb[2]});
    const double mn = std::min({rgb[0], rgb[1], rgb[2]});
    const double l = (mx + mn) / 2.0;
    const double d = mx - mn;
    double h = std::nan(""), s = 0.0;
    if (d > 1e-9) {
        s = (l == 0.0 || l == 1.0) ? 0.0 : (mx - l) / std::min(l, 1.0 - l);
        if (mx == rgb[0])
            h = (rgb[1] - rgb[2]) / d + (rgb[1] < rgb[2] ? 6.0 : 0.0);
        else if (mx == rgb[1])
            h = (rgb[2] - rgb[0]) / d + 2.0;
        else
            h = (rgb[0] - rgb[1]) / d + 4.0;
        h *= 60.0;
    }
    return {h, s, l};
}

Vec3 hsl_to_srgb(const Vec3& hsl) {
    const double h = std::isnan(hsl[0]) ? 0.0 : normalize_hue(hsl[0]);
    const auto f = [&](double n) {
        const double k = std::fmod(n + h / 30.0, 12.0);
        const double a = hsl[1] * std::min(hsl[2], 1.0 - hsl[2]);
        return hsl[2] - a * std::max(-1.0, std::min({k - 3.0, 9.0 - k, 1.0}));
    };
    return {f(0.0), f(8.0), f(4.0)};
}

Vec3 srgb_to_hwb(const Vec3& rgb) {
    const Vec3 hsl = srgb_to_hsl(rgb);
    const double w = std::min({rgb[0], rgb[1], rgb[2]});
    const double b = 1.0 - std::max({rgb[0], rgb[1], rgb[2]});
    return {w + b >= 1.0 - 1e-9 ? std::nan("") : hsl[0], w, b};
}

Vec3 hwb_to_srgb(const Vec3& hwb) {
    const double sum = hwb[1] + hwb[2];
    if (sum >= 1.0) {
        const double grey = hwb[1] / sum;
        return {grey, grey, grey};
    }
    const Vec3 pure = hsl_to_srgb({hwb[0], 1.0, 0.5});
    const double k = 1.0 - hwb[1] - hwb[2];
    return {pure[0] * k + hwb[1], pure[1] * k + hwb[1], pure[2] * k + hwb[1]};
}

// ── Space dispatch. Polar spaces keep the hue at the index hue_index() names.

int hue_index(GradientColorSpace space) {
    return (space == GradientColorSpace::hsl || space == GradientColorSpace::hwb) ? 0 : 2;
}

Vec3 from_srgb(GradientColorSpace space, const Vec3& rgb) {
    const Vec3 lin = each(rgb, srgb_to_linear);
    switch (space) {
    case GradientColorSpace::srgb:
        return rgb;
    case GradientColorSpace::srgb_linear:
        return lin;
    case GradientColorSpace::display_p3:
        return each(mul(kXyz65ToLinP3, mul(kLinSrgbToXyz65, lin)), srgb_from_linear);
    case GradientColorSpace::a98_rgb:
        return each(mul(kXyz65ToLinA98, mul(kLinSrgbToXyz65, lin)), a98_from_linear);
    case GradientColorSpace::prophoto_rgb:
        return each(mul(kXyz50ToLinProphoto, mul(kXyz65ToXyz50, mul(kLinSrgbToXyz65, lin))),
                    prophoto_from_linear);
    case GradientColorSpace::rec2020:
        return each(mul(kXyz65ToLin2020, mul(kLinSrgbToXyz65, lin)), rec2020_from_linear);
    case GradientColorSpace::lab:
        return xyz50_to_lab(mul(kXyz65ToXyz50, mul(kLinSrgbToXyz65, lin)));
    case GradientColorSpace::oklab:
        return lin_srgb_to_oklab(lin);
    case GradientColorSpace::hsl:
        return srgb_to_hsl(rgb);
    case GradientColorSpace::hwb:
        return srgb_to_hwb(rgb);
    case GradientColorSpace::lch:
        return to_polar(xyz50_to_lab(mul(kXyz65ToXyz50, mul(kLinSrgbToXyz65, lin))), 1e-2);
    case GradientColorSpace::oklch:
        return to_polar(lin_srgb_to_oklab(lin), 1e-4);
    }
    return rgb;
}

Vec3 to_srgb(GradientColorSpace space, const Vec3& v) {
    switch (space) {
    case GradientColorSpace::srgb:
        return v;
    case GradientColorSpace::srgb_linear:
        return each(v, srgb_from_linear);
    case GradientColorSpace::display_p3:
        return each(mul(kXyz65ToLinSrgb, mul(kLinP3ToXyz65, each(v, srgb_to_linear))),
                    srgb_from_linear);
    case GradientColorSpace::a98_rgb:
        return each(mul(kXyz65ToLinSrgb, mul(kLinA98ToXyz65, each(v, a98_to_linear))),
                    srgb_from_linear);
    case GradientColorSpace::prophoto_rgb:
        return each(mul(kXyz65ToLinSrgb,
                        mul(kXyz50ToXyz65, mul(kLinProphotoToXyz50, each(v, prophoto_to_linear)))),
                    srgb_from_linear);
    case GradientColorSpace::rec2020:
        return each(mul(kXyz65ToLinSrgb, mul(kLin2020ToXyz65, each(v, rec2020_to_linear))),
                    srgb_from_linear);
    case GradientColorSpace::lab:
        return each(mul(kXyz65ToLinSrgb, mul(kXyz50ToXyz65, lab_to_xyz50(v))), srgb_from_linear);
    case GradientColorSpace::oklab:
        return each(oklab_to_lin_srgb(v), srgb_from_linear);
    case GradientColorSpace::hsl:
        return hsl_to_srgb(v);
    case GradientColorSpace::hwb:
        return hwb_to_srgb(v);
    case GradientColorSpace::lch:
        return each(mul(kXyz65ToLinSrgb, mul(kXyz50ToXyz65, lab_to_xyz50(from_polar(v)))),
                    srgb_from_linear);
    case GradientColorSpace::oklch:
        return each(oklab_to_lin_srgb(from_polar(v)), srgb_from_linear);
    }
    return v;
}

// CSS Color 4 §12.4: adjust the pair so plain linear interpolation travels the
// requested way round the wheel.
void fix_hues(double& h1, double& h2, GradientHueMethod method) {
    const double d = h2 - h1;
    switch (method) {
    case GradientHueMethod::shorter:
        if (d > 180.0)
            h1 += 360.0;
        else if (d < -180.0)
            h2 += 360.0;
        break;
    case GradientHueMethod::longer:
        if (d > 0.0 && d < 180.0)
            h1 += 360.0;
        else if (d > -180.0 && d <= 0.0)
            h2 += 360.0;
        break;
    case GradientHueMethod::increasing:
        if (d < 0.0)
            h2 += 360.0;
        break;
    case GradientHueMethod::decreasing:
        if (d > 0.0)
            h1 += 360.0;
        break;
    }
}

} // namespace

Color interpolate_gradient_color(const Color& a, const Color& b, float t,
                                 GradientInterpolation method) {
    const GradientColorSpace space = method.space;
    Vec3 ca = from_srgb(space, {a.r, a.g, a.b});
    Vec3 cb = from_srgb(space, {b.r, b.g, b.b});
    const bool polar = is_polar(space);
    const int hi = polar ? hue_index(space) : -1;
    if (polar) {
        // A powerless hue takes the other stop's, so a fade from white to blue
        // stays blue all the way rather than sweeping in from red.
        if (std::isnan(ca[hi]))
            ca[hi] = std::isnan(cb[hi]) ? 0.0 : cb[hi];
        if (std::isnan(cb[hi]))
            cb[hi] = ca[hi];
        fix_hues(ca[hi], cb[hi], method.hue);
    }
    // Premultiply every component but the hue.
    const double aa = a.a, ab = b.a;
    for (int i = 0; i < 3; ++i) {
        if (i == hi)
            continue;
        ca[i] *= aa;
        cb[i] *= ab;
    }
    const double tt = t;
    const double alpha = aa + (ab - aa) * tt;
    Vec3 mixed{};
    for (int i = 0; i < 3; ++i) {
        mixed[i] = ca[i] + (cb[i] - ca[i]) * tt;
        if (i != hi && alpha > 0.0)
            mixed[i] /= alpha;
    }
    if (polar)
        mixed[hi] = normalize_hue(mixed[hi]);
    const Vec3 rgb = to_srgb(space, mixed);
    const auto clip = [](double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); };
    return Color::rgba(clip(rgb[0]), clip(rgb[1]), clip(rgb[2]), clip(alpha));
}

void resample_gradient_stops(const Color* colors, const float* positions, int count,
                             GradientInterpolation method, int samples_per_segment,
                             std::vector<Color>& out_colors, std::vector<float>& out_positions) {
    out_colors.clear();
    out_positions.clear();
    if (count <= 0 || colors == nullptr)
        return;
    const auto position_of = [&](int i) {
        if (positions != nullptr)
            return positions[i];
        return count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f;
    };
    const int samples = std::max(1, samples_per_segment);
    const bool identity = method == GradientInterpolation{};
    out_colors.reserve(static_cast<std::size_t>(identity ? count : (count - 1) * samples + 1));
    out_positions.reserve(out_colors.capacity());
    out_colors.push_back(colors[0]);
    out_positions.push_back(position_of(0));
    for (int i = 1; i < count; ++i) {
        const float p0 = position_of(i - 1), p1 = position_of(i);
        // A hard stop (two stops at one position) has no curve to follow.
        if (!identity && p1 > p0) {
            for (int s = 1; s < samples; ++s) {
                const float t = static_cast<float>(s) / static_cast<float>(samples);
                out_colors.push_back(
                    interpolate_gradient_color(colors[i - 1], colors[i], t, method));
                out_positions.push_back(p0 + (p1 - p0) * t);
            }
        }
        out_colors.push_back(colors[i]);
        out_positions.push_back(p1);
    }
}

} // namespace pulp::canvas
