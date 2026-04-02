#pragma once

#include "generated_gain.hpp"
#include <pulp/dsl/faust_processor.hpp>

namespace pulp::examples {

using FaustGainProcessor = dsl::FaustProcessor<FaustGainDsp>;

inline std::unique_ptr<format::Processor> create_faust_gain() {
    return std::make_unique<FaustGainProcessor>();
}

} // namespace pulp::examples
