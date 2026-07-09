#pragma once

// Factory seams for the traced effect chain. Each real example plugin is
// wrapped in its own translation unit (gain.cpp / filter.cpp / compressor.cpp)
// so the driver never includes more than one plugin header at a time: the
// example plugins share unscoped `pulp::examples::kBypass` enumerators that
// would otherwise collide in a single TU.

#include <memory>
#include <pulp/format/processor.hpp>

namespace tracechain {

std::unique_ptr<pulp::format::Processor> make_gain();
std::unique_ptr<pulp::format::Processor> make_filter();
std::unique_ptr<pulp::format::Processor> make_compressor();

}  // namespace tracechain
