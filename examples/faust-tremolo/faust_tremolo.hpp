#pragma once

#include "reference_tremolo.hpp"
#include <pulp/dsl/faust_processor.hpp>

namespace pulp::examples {

using FaustTremoloProcessor = dsl::FaustProcessor<FaustTremoloDsp>;

inline std::unique_ptr<format::Processor> create_faust_tremolo() {
    return std::make_unique<FaustTremoloProcessor>();
}

} // namespace pulp::examples
