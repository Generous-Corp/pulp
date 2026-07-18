#pragma once

#include <pulp/timebase/compiled_tempo_map.hpp>

#include <cstdlib>
#include <memory>
#include <utility>

template <typename Points>
pulp::timebase::CompiledTempoMap
require_compiled_tempo_map(const Points& points, pulp::timebase::RationalRate rate) {
    auto result = pulp::timebase::CompiledTempoMap::compile(points, rate);
    if (!result)
        std::abort();
    return std::move(result).value();
}

template <typename Points>
std::shared_ptr<const pulp::timebase::CompiledTempoMap>
shared_compiled_tempo_map(const Points& points, pulp::timebase::RationalRate rate) {
    return std::make_shared<const pulp::timebase::CompiledTempoMap>(
        require_compiled_tempo_map(points, rate));
}
