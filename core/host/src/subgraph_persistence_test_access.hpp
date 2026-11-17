#pragma once

#include <cstddef>

namespace pulp::host {

struct SubgraphPersistenceTestAccess {
    // The `call`th record rebind from now on (1 = the next one) throws
    // std::bad_alloc; 0 disarms. A restore rebinds every record once for the
    // candidate graph and once more for the live graph, so `records + 1`
    // fails the live graph's first record after the candidate succeeded.
    static void fail_rebind_call(std::size_t call) noexcept;
};

} // namespace pulp::host
