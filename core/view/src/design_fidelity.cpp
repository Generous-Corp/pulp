// core/view/src/design_fidelity.cpp
#include <pulp/view/design_fidelity.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace pulp::view {
namespace {

// Read a numeric node attribute (0 if absent / unparsable).
float attr_f(const IRNode& n, const char* key) {
    auto it = n.attributes.find(key);
    return it != n.attributes.end() ? std::strtof(it->second.c_str(), nullptr) : 0.0f;
}

bool is_bleed_sprite(const IRNode& n) {
    return n.style.render_bounds.has_value() ||
           (n.attributes.count("asset_bleed") && n.attributes.at("asset_bleed") == "1");
}

}  // namespace

std::optional<FidelityIssue> check_image_sizing_fidelity(const FidelityContext& ctx) {
    const IRNode& node = ctx.node;
    if (!is_bleed_sprite(node)) return std::nullopt;  // ordinary images fill their box

    const float png_w = attr_f(node, "png_natural_w");
    const float png_h = attr_f(node, "png_natural_h");
    if (png_w <= 0.0f || png_h <= 0.0f) {
        return FidelityIssue{ctx.node_id, node.name, "aspect-unverified",
            "bleed sprite has no source PNG dimensions; aspect could not be verified"};
    }
    if (ctx.emitted_w <= 0.0f || ctx.emitted_h <= 0.0f) return std::nullopt;

    const float png_aspect = png_w / png_h;
    const float emitted_aspect = ctx.emitted_w / ctx.emitted_h;
    const float rel = std::fabs(emitted_aspect - png_aspect) / png_aspect;
    if (rel > 0.05f) {
        std::ostringstream d;
        d << "emitted aspect " << emitted_aspect << " diverges from source PNG aspect "
          << png_aspect << " (" << static_cast<int>(rel * 100.0f + 0.5f)
          << "% off) — sprite skewed";
        return FidelityIssue{ctx.node_id, node.name, "skew", d.str()};
    }
    return std::nullopt;
}

std::optional<FidelityIssue> check_gross_size_divergence(const FidelityContext& ctx) {
    const IRNode& node = ctx.node;
    if (node.layout.width_mode  != SizingMode::fixed) return std::nullopt;
    if (node.layout.height_mode != SizingMode::fixed) return std::nullopt;
    if (node.style.position.has_value() && *node.style.position == "absolute")
        return std::nullopt;
    if (node.layout.display.has_value() && *node.layout.display == "none")
        return std::nullopt;

    const float src_w = node.style.width.value_or(0.0f);
    const float src_h = node.style.height.value_or(0.0f);
    if (src_w <= 0.0f || src_h <= 0.0f) return std::nullopt;
    if (ctx.emitted_w <= 0.0f || ctx.emitted_h <= 0.0f) return std::nullopt;

    auto ratio = [](float a, float b) { return std::max(a, b) / std::min(a, b); };
    const float rw = ratio(ctx.emitted_w, src_w);
    const float rh = ratio(ctx.emitted_h, src_h);
    constexpr float kMaxRatio = 3.0f;
    if (rw > kMaxRatio || rh > kMaxRatio) {
        std::ostringstream d;
        d << "fixed-sized node emitted box diverges from source: "
          << "W " << rw << "x (source " << src_w << " emitted " << ctx.emitted_w << "px), "
          << "H " << rh << "x (source " << src_h << " emitted " << ctx.emitted_h << "px)";
        return FidelityIssue{ctx.node_id, node.name, "gross-size", d.str()};
    }
    return std::nullopt;
}

// ── Registry: the single place to add a new invariant ────────────────────────
// Each entry pairs a check with the element kind it applies to. A check runs
// ONLY for its element (an image's emitted box legitimately differs from its
// style box, so the container gross-size test must not see images, etc.).
// Adding an invariant = one row here + its function above.
namespace {
using CheckFn = std::optional<FidelityIssue> (*)(const FidelityContext&);
struct RegisteredCheck { FidelityElement applies_to; CheckFn fn; };
constexpr std::array<RegisteredCheck, 2> kChecks = {{
    {FidelityElement::image,     &check_image_sizing_fidelity},
    {FidelityElement::container, &check_gross_size_divergence},
}};
}  // namespace

void run_fidelity_checks(const FidelityContext& ctx, std::vector<FidelityIssue>& sink) {
    for (const auto& c : kChecks)
        if (c.applies_to == ctx.element)
            if (auto issue = c.fn(ctx)) sink.push_back(std::move(*issue));
}

}  // namespace pulp::view
