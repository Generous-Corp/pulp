#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#include <cstddef>

namespace pulp::test {
struct ExampleParams {
    const char* name;
    std::size_t count;
    std::size_t refused;
};
ExampleParams params_of_gain();
ExampleParams params_of_effect();
ExampleParams params_of_drums();
} // namespace pulp::test
