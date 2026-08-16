#include <pulp/music/pattern_development.hpp>

#include <array>

namespace {

using namespace pulp::music;

constexpr bool compile_contract() noexcept {
    DevelopmentPattern<4> a;
    DevelopmentPattern<4> b;
    if (a.insert({1, {0}, 1000, PatternEventRole::anchor}) != PatternDevelopmentError::none)
        return false;
    if (b.insert({2, {100}, 500, PatternEventRole::fill}) != PatternDevelopmentError::none)
        return false;
    return select_pattern_density(a, {1, 0, {}}) &&
           apply_regional_fill(a, b, {{0}, {200}, 1, 0, {}}) && morph_patterns(a, b, {500, 0, {}});
}

static_assert(compile_contract());

} // namespace
