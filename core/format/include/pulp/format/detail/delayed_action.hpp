#pragma once

#include <functional>
#include <memory>

namespace pulp::format::detail {

struct DelayedAction {
    int delay = 30;
    std::function<void()> action_fn;
    std::function<void()> close_fn;

    std::shared_ptr<int> frame = std::make_shared<int>(0);
    std::shared_ptr<bool> captured = std::make_shared<bool>(false);

    void operator()() {
        if (*captured) return;
        ++(*frame);
        if (*frame < delay) return;
        *captured = true;
        if (action_fn) action_fn();
        if (close_fn) close_fn();
    }
};

}  // namespace pulp::format::detail
