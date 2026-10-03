#pragma once

#include <cstddef>

namespace pulp::test {

// Private, test-only event probe.  These explicit seams make the contract's
// portability controls observable on hosts where pthread interposition is not
// available.  They do not instrument or alter the production streaming ABI.
class RtContractProbe {
  public:
    RtContractProbe() noexcept;
    ~RtContractProbe() noexcept;

    RtContractProbe(const RtContractProbe&) = delete;
    RtContractProbe& operator=(const RtContractProbe&) = delete;

    std::size_t allocation_count() const noexcept {
        return allocation_count_;
    }
    std::size_t allocated_bytes() const noexcept {
        return allocated_bytes_;
    }
    std::size_t lock_events() const noexcept {
        return lock_events_;
    }
    std::size_t blocking_events() const noexcept {
        return blocking_events_;
    }
    std::size_t stale_result_events() const noexcept {
        return stale_result_events_;
    }
    std::size_t alias_rejection_events() const noexcept {
        return alias_rejection_events_;
    }

  private:
    friend void rt_contract_probe_record_allocation(std::size_t bytes) noexcept;
    friend void rt_contract_probe_record_lock() noexcept;
    friend void rt_contract_probe_record_blocking() noexcept;
    friend void rt_contract_probe_record_stale_result() noexcept;
    friend void rt_contract_probe_record_alias_rejection() noexcept;

    RtContractProbe* previous_ = nullptr;
    std::size_t allocation_count_ = 0;
    std::size_t allocated_bytes_ = 0;
    std::size_t lock_events_ = 0;
    std::size_t blocking_events_ = 0;
    std::size_t stale_result_events_ = 0;
    std::size_t alias_rejection_events_ = 0;
};

namespace detail {
inline thread_local RtContractProbe* current_rt_contract_probe = nullptr;
} // namespace detail

inline RtContractProbe::RtContractProbe() noexcept : previous_(detail::current_rt_contract_probe) {
    detail::current_rt_contract_probe = this;
}

inline RtContractProbe::~RtContractProbe() noexcept {
    detail::current_rt_contract_probe = previous_;
}

inline void rt_contract_probe_record_allocation(std::size_t bytes) noexcept {
    if (detail::current_rt_contract_probe != nullptr) {
        ++detail::current_rt_contract_probe->allocation_count_;
        detail::current_rt_contract_probe->allocated_bytes_ += bytes;
    }
}

inline void rt_contract_probe_record_lock() noexcept {
    if (detail::current_rt_contract_probe != nullptr) {
        ++detail::current_rt_contract_probe->lock_events_;
    }
}

inline void rt_contract_probe_record_blocking() noexcept {
    if (detail::current_rt_contract_probe != nullptr) {
        ++detail::current_rt_contract_probe->blocking_events_;
    }
}

inline void rt_contract_probe_record_stale_result() noexcept {
    if (detail::current_rt_contract_probe != nullptr) {
        ++detail::current_rt_contract_probe->stale_result_events_;
    }
}

inline void rt_contract_probe_record_alias_rejection() noexcept {
    if (detail::current_rt_contract_probe != nullptr) {
        ++detail::current_rt_contract_probe->alias_rejection_events_;
    }
}

inline bool rt_contract_probe_active() noexcept {
    return detail::current_rt_contract_probe != nullptr;
}

} // namespace pulp::test
