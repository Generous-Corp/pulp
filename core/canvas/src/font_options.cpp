// font_options.cpp — Pulp #2163 follow-up, Phase 1 / Slice 1.1.a.
//
// Hash implementation for FontOptions. Every field contributes; the
// rule is "every cache keys on the full blob, never on a subset."

#include "pulp/canvas/font_options.hpp"

#include <bit>
#include <cstddef>
#include <cstring>

namespace pulp::canvas {

namespace {

inline std::size_t mix(std::size_t seed, std::size_t v) noexcept {
    // 0x9e3779b97f4a7c15 — the golden-ratio constant, standard hash combine.
    return seed ^ (v + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
}

inline std::size_t hash_float(float f) noexcept {
    // Hash the bit pattern so +0.0 == -0.0 collide consistently; NaN
    // hashes are intentionally not normalised because two NaN-bearing
    // FontOptions values that hash differently is benign for cache keys.
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return std::hash<std::uint32_t>{}(bits);
}

inline std::size_t hash_string(const std::string& s) noexcept {
    return std::hash<std::string>{}(s);
}

} // namespace

std::size_t FontOptions::hash() const noexcept {
    std::size_t h = 0;

    for (const auto& fam : family_stack) {
        h = mix(h, hash_string(fam));
    }
    h = mix(h, hash_float(weight));
    h = mix(h, hash_float(width));
    h = mix(h, std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(slant)));
    h = mix(h, hash_float(oblique_angle));
    h = mix(h, hash_float(size));

    for (const auto& feat : features) {
        h = mix(h, std::hash<std::uint32_t>{}(feat.tag));
        h = mix(h, std::hash<std::int32_t>{}(feat.value));
    }

    for (const auto& axis : variation_axes) {
        h = mix(h, std::hash<std::uint32_t>{}(axis.tag));
        h = mix(h, hash_float(axis.value));
    }

    h = mix(h, hash_string(locale));
    h = mix(h, std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(direction)));
    h = mix(h, hash_float(letter_spacing));
    h = mix(h, hash_float(word_spacing));
    h = mix(h, std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(hinting_mode)));
    h = mix(h, std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(aa_mode)));
    h = mix(h, std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(color_font_mode)));

    h = mix(h, font_synthesis.weight ? 0xA1u : 0xB1u);
    h = mix(h, font_synthesis.slant  ? 0xA2u : 0xB2u);
    h = mix(h, font_synthesis.width  ? 0xA3u : 0xB3u);

    h = mix(h, std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(fallback_mode)));
    h = mix(h, std::hash<FontScopeId>{}(scope));
    h = mix(h, std::hash<std::uint64_t>{}(registry_generation));

    return h;
}

} // namespace pulp::canvas
