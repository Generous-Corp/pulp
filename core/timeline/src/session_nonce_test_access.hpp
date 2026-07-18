#pragma once

#include <cstdint>

namespace pulp::timeline::detail {

class SessionNonceTestAccess {
  public:
    static std::uint64_t exchange_next(std::uint64_t value) noexcept;
};

} // namespace pulp::timeline::detail
