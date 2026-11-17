// SPDX-License-Identifier: MIT
// Test-only Windows callback traps. The test executable's import address table
// is redirected for the heap, lock, wait, pipe I/O and process-control entry
// points, and each call made inside a Scope on the calling thread is counted
// by kind. Pulp and the static CRT are linked into the executable, so their
// calls into the system resolve through this table.
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace pulp::test::windows_traps {

enum class Kind : std::size_t { allocation, lock, wait, io, handle, process, count };

struct Counts {
    std::array<unsigned, static_cast<std::size_t>(Kind::count)> calls{};
    unsigned operator[](Kind kind) const noexcept {
        return calls[static_cast<std::size_t>(kind)];
    }
    unsigned total() const noexcept {
        unsigned sum = 0;
        for (const auto value : calls)
            sum += value;
        return sum;
    }
};

/// Redirects the traps once. Returns the import names that were redirected.
const std::vector<std::string>& install();

/// Counts trapped calls on the constructing thread until destruction.
class Scope {
  public:
    Scope() noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Counts counts() const noexcept;

  private:
    Counts* previous_;
    Counts counts_;
};

} // namespace pulp::test::windows_traps
