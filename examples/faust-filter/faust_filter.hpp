#pragma once

#include "generated_filter.hpp"
#include <pulp/dsl/faust_processor.hpp>

namespace pulp::examples {

using FaustFilterProcessor = dsl::FaustProcessor<FaustFilterDsp>;

inline std::unique_ptr<format::Processor> create_faust_filter() {
    return std::make_unique<FaustFilterProcessor>();
}

} // namespace pulp::examples
