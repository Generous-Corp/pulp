#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace pulp::cli::control {

class OperationDeadline {
  public:
    using Clock = std::chrono::steady_clock;

    explicit OperationDeadline(std::chrono::milliseconds budget,
                               Clock::time_point started_at = Clock::now())
        : deadline_(started_at + budget) {}

    std::chrono::milliseconds remaining(Clock::time_point now = Clock::now()) const {
        if (now >= deadline_)
            return std::chrono::milliseconds::zero();
        return std::max(std::chrono::ceil<std::chrono::milliseconds>(deadline_ - now),
                        std::chrono::milliseconds{1});
    }

    std::int64_t unix_deadline_ms(
        std::chrono::system_clock::time_point system_now,
        Clock::time_point steady_now = Clock::now()) const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(system_now.time_since_epoch())
                   .count() +
               remaining(steady_now).count();
    }

  private:
    Clock::time_point deadline_;
};

} // namespace pulp::cli::control
