#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace pulp::playback::detail {

struct BudgetedStableMergeState {
    struct Step {
        bool complete = false;
        std::size_t work_units = 0;
    };

    template <typename T>
    void reset(std::vector<T>& scratch) noexcept {
        scratch.clear();
        width = 1;
        left = 0;
        mid = 0;
        right = 0;
        i = 0;
        j = 0;
        pair_active = false;
        clearing_source = false;
    }

    template <typename T, typename Less>
    Step step(std::vector<T>& values, std::vector<T>& scratch, Less less) {
        if (values.size() <= 1 || width >= values.size())
            return {.complete = true};
        if (clearing_source) {
            scratch.pop_back();
            if (scratch.empty()) {
                clearing_source = false;
                width *= 2;
                left = 0;
            }
            return {.work_units = 1};
        }
        if (left >= values.size()) {
            values.swap(scratch);
            clearing_source = true;
            return {};
        }
        if (!pair_active) {
            mid = std::min(left + width, values.size());
            right = std::min(left + 2 * width, values.size());
            i = left;
            j = mid;
            pair_active = true;
            return {};
        }
        if (i < mid && (j >= right || !less(values[j], values[i])))
            scratch.push_back(std::move(values[i++]));
        else
            scratch.push_back(std::move(values[j++]));
        if (i == mid && j == right) {
            left = right;
            pair_active = false;
        }
        return {.work_units = 1};
    }

    std::size_t width = 1;
    std::size_t left = 0;
    std::size_t mid = 0;
    std::size_t right = 0;
    std::size_t i = 0;
    std::size_t j = 0;
    bool pair_active = false;
    bool clearing_source = false;
};

} // namespace pulp::playback::detail
