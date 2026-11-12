#pragma once

#include <cstddef>

namespace pulp::test {

class RtAllocationProbe {
public:
    RtAllocationProbe() noexcept;
    ~RtAllocationProbe() noexcept;

    RtAllocationProbe(const RtAllocationProbe&) = delete;
    RtAllocationProbe& operator=(const RtAllocationProbe&) = delete;

    std::size_t allocation_count() const noexcept { return allocation_count_; }
    std::size_t allocated_bytes() const noexcept { return allocated_bytes_; }
    bool saw_allocation() const noexcept { return allocation_count_ != 0; }

    // The probe's positive control. Allocates twice inside a fresh probe, once
    // through ::operator new and once through a new-expression whose pointer
    // escapes through a volatile, so neither can be optimised away, and
    // reports whether both were counted with their sizes. False means
    // allocations in this binary bypass the probe (its source is not linked, or
    // another replacement of operator new wins), so an absence assertion such
    // as REQUIRE_FALSE(probe.saw_allocation()) would pass without looking.
    static bool self_check() noexcept;

private:
    friend void rt_allocation_probe_record(std::size_t bytes) noexcept;

    RtAllocationProbe* previous_ = nullptr;
    std::size_t allocation_count_ = 0;
    std::size_t allocated_bytes_ = 0;
};

void rt_allocation_probe_record(std::size_t bytes) noexcept;
bool rt_allocation_probe_active() noexcept;

}  // namespace pulp::test
