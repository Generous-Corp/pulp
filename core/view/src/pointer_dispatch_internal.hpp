#pragma once

#include <cstdint>

namespace pulp::view::detail {

// One token spans the target-to-root callbacks produced by a single native
// pointer delivery. The JS bridge uses it to carry stopPropagation across
// separate evaluate() calls without coupling the public MouseEvent ABI to the
// web-compat runtime. Nested native delivery restores the outer token.
inline thread_local std::uint32_t current_dom_pointer_token = 0;
inline thread_local std::uint32_t next_dom_pointer_token = 0;

class ScopedDomPointerToken {
public:
    ScopedDomPointerToken() noexcept
        : previous_(current_dom_pointer_token) {
        do {
            ++next_dom_pointer_token;
        } while (next_dom_pointer_token == 0);
        current_dom_pointer_token = next_dom_pointer_token;
    }

    ~ScopedDomPointerToken() {
        current_dom_pointer_token = previous_;
    }

    ScopedDomPointerToken(const ScopedDomPointerToken&) = delete;
    ScopedDomPointerToken& operator=(const ScopedDomPointerToken&) = delete;

private:
    std::uint32_t previous_;
};

inline std::uint32_t dom_pointer_token() noexcept {
    return current_dom_pointer_token;
}

} // namespace pulp::view::detail
