#include "chain.hpp"

#include "pulp_effect.hpp"

namespace tracechain {

std::unique_ptr<pulp::format::Processor> make_filter() {
    return pulp::examples::create_pulp_effect();
}

}  // namespace tracechain
