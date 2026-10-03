#pragma once

#include <cstdint>
#include <string>

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

// The element id of the open overlay a wheel resolved into, while that wheel's
// DOM dispatch runs, or null outside one. The wheel registrar adds it to the
// payload so the web-compat bubble stops at the overlay element, matching the
// native containment in deliver_mouse_wheel. A thread-local rather than a
// MouseEvent field for the same reason as the token above.
inline thread_local const std::string* current_wheel_boundary_id = nullptr;

class ScopedWheelBoundary {
public:
    explicit ScopedWheelBoundary(const std::string* id) noexcept
        : previous_(current_wheel_boundary_id) {
        current_wheel_boundary_id = (id && !id->empty()) ? id : nullptr;
    }
    ~ScopedWheelBoundary() { current_wheel_boundary_id = previous_; }
    ScopedWheelBoundary(const ScopedWheelBoundary&) = delete;
    ScopedWheelBoundary& operator=(const ScopedWheelBoundary&) = delete;

private:
    const std::string* previous_;
};

inline const std::string* wheel_boundary_id() noexcept {
    return current_wheel_boundary_id;
}

} // namespace pulp::view::detail
