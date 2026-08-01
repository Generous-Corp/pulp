#pragma once

#include <cstddef>

namespace pulp::inspect::detail {

/// Internal lifecycle probe for inspector server tests. Dynamic-module hosts
/// must likewise wait for this count to reach zero before unloading code.
std::size_t active_inspector_cleanup_workers_for_testing() noexcept;

} // namespace pulp::inspect::detail
