#pragma once

#include <type_traits>

namespace pulp::signal {

/// One sample of explicit signal memory.
///
/// `publish()` exposes the retained sample without changing it. `commit()`
/// replaces that sample after the current equation has finished reading it.
/// `process()` is the scalar convenience form for acyclic processing.
template <typename SampleType = float> class UnitDelayT {
    static_assert(std::is_floating_point_v<SampleType>,
                  "UnitDelayT requires a floating-point sample type");

  public:
    [[nodiscard]] SampleType publish() const noexcept {
        return state_;
    }

    void commit(SampleType input) noexcept {
        state_ = input;
    }

    [[nodiscard]] SampleType process(SampleType input) noexcept {
        const SampleType output = publish();
        commit(input);
        return output;
    }

    void reset() noexcept {
        state_ = SampleType{};
    }

  private:
    SampleType state_{};
};

using UnitDelay = UnitDelayT<float>;
using UnitDelay64 = UnitDelayT<double>;

static_assert(sizeof(UnitDelay) == sizeof(float));
static_assert(alignof(UnitDelay) == alignof(float));

} // namespace pulp::signal
