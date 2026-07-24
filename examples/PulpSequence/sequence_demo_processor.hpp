#pragma once

#include <pulp/format/processor.hpp>

#include <memory>

namespace pulp::examples {

std::unique_ptr<format::Processor> create_pulp_sequence();

} // namespace pulp::examples
