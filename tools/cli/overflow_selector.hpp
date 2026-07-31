#pragma once

#include <string>

namespace pulp::cli {

struct OverflowSelectorValidation {
    bool valid = false;
    bool is_off_switch = false;
    std::string error;
};

// Validate the exact JSON grammar accepted by GitHub Actions `fromJSON()` for
// runner selectors: one non-empty safe ASCII label, or a non-empty array of
// such labels. The off-switch is recognized after JSON escape decoding.
OverflowSelectorValidation validate_overflow_selector(const std::string& text);
bool is_safe_runner_label(const std::string& label);

}  // namespace pulp::cli
