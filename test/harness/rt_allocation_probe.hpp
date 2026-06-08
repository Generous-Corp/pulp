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

private:
    friend void rt_allocation_probe_record(std::size_t bytes) noexcept;

    RtAllocationProbe* previous_ = nullptr;
    std::size_t allocation_count_ = 0;
    std::size_t allocated_bytes_ = 0;
};

void rt_allocation_probe_record(std::size_t bytes) noexcept;
bool rt_allocation_probe_active() noexcept;

}  // namespace pulp::test
