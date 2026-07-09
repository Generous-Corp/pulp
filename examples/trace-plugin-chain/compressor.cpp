#include "chain.hpp"

#include "pulp_compressor.hpp"

namespace tracechain {

std::unique_ptr<pulp::format::Processor> make_compressor() {
    return pulp::examples::create_pulp_compressor();
}

}  // namespace tracechain
