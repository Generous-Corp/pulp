#include "chain.hpp"

#include "pulp_gain.hpp"

namespace tracechain {

std::unique_ptr<pulp::format::Processor> make_gain() {
    return pulp::examples::create_pulp_gain();
}

}  // namespace tracechain
