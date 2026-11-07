#pragma once

/// @file fixed_function.hpp
/// `FixedFunction<Signature, Capacity>` - a move-only callable wrapper with
/// inline storage that never touches the heap.
///
/// `std::function` may allocate when it is constructed or copied and requires a
/// copyable target, so it does not belong on a realtime path. `FixedFunction`
/// stores the target inside the object. A target that does not fit is a compile
/// error, not a silent allocation, so a realtime queue of callables stays
/// allocation-free by construction.
///
/// ```cpp
/// FixedFunction<void(int), 32> fn = [gain = &gain_](int v) { gain->store(v); };
/// fn(7);
/// ```
///
/// Contract:
///  - Construction, move, call and destruction never allocate, lock or throw.
///  - The target must fit `Capacity` bytes, need no stricter alignment than
///    `Alignment`, and be nothrow move-constructible. Each is a `static_assert`.
///  - Move-only. Move-only targets (a lambda owning a `unique_ptr`) are allowed.
///  - Invoking an empty `FixedFunction` is undefined behaviour. Test with
///    `explicit operator bool` first when it can be empty.
///  - Not thread-safe: one thread at a time may call, move or destroy an object,
///    like any non-atomic value. Hand it between threads through a queue.
///
/// The target is destroyed with the wrapper, so destroying a `FixedFunction` on
/// the audio thread runs the target's destructor there. Capture trivially
/// destructible state (pointers, numbers) for objects that live on the audio
/// thread.

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace pulp::runtime {

template <typename Signature, std::size_t Capacity = 32,
          std::size_t Alignment = alignof(std::max_align_t)>
class FixedFunction;

template <typename R, typename... Args, std::size_t Capacity, std::size_t Alignment>
class FixedFunction<R(Args...), Capacity, Alignment> {
    static_assert(Capacity >= sizeof(void*), "Capacity must hold at least a pointer");
    static_assert(Alignment > 0 && (Alignment & (Alignment - 1)) == 0,
                  "Alignment must be a power of two");

  public:
    static constexpr std::size_t capacity = Capacity;
    static constexpr std::size_t alignment = Alignment;

    /// True if a callable of type `F` can be stored.
    template <typename F>
    static constexpr bool fits =
        sizeof(std::decay_t<F>) <= Capacity && alignof(std::decay_t<F>) <= Alignment &&
        std::is_nothrow_move_constructible_v<std::decay_t<F>>;

    FixedFunction() noexcept = default;
    FixedFunction(std::nullptr_t) noexcept {}

    template <typename F, typename D = std::decay_t<F>,
              typename = std::enable_if_t<!std::is_same_v<D, FixedFunction> &&
                                          !std::is_same_v<D, std::nullptr_t> &&
                                          std::is_invocable_r_v<R, D&, Args...>>>
    FixedFunction(F&& f) noexcept(
        std::is_nothrow_constructible_v<D, F&&>) { // NOLINT: implicit like std::function
        static_assert(sizeof(D) <= Capacity,
                      "callable is too large for this FixedFunction: capture less (pointers or "
                      "indices instead of objects) or raise Capacity");
        static_assert(alignof(D) <= Alignment,
                      "callable needs stricter alignment than this FixedFunction provides");
        static_assert(std::is_nothrow_move_constructible_v<D>,
                      "callable must be nothrow move constructible so moving a FixedFunction "
                      "cannot throw");
        ::new (static_cast<void*>(storage_)) D(std::forward<F>(f));
        vtable_ = &vtable_for<D>;
    }

    FixedFunction(const FixedFunction&) = delete;
    FixedFunction& operator=(const FixedFunction&) = delete;

    FixedFunction(FixedFunction&& other) noexcept {
        take(other);
    }

    FixedFunction& operator=(FixedFunction&& other) noexcept {
        if (this != &other) {
            reset();
            take(other);
        }
        return *this;
    }

    FixedFunction& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    template <typename F, typename D = std::decay_t<F>,
              typename = std::enable_if_t<!std::is_same_v<D, FixedFunction> &&
                                          !std::is_same_v<D, std::nullptr_t> &&
                                          std::is_invocable_r_v<R, D&, Args...>>>
    FixedFunction& operator=(F&& f) noexcept(std::is_nothrow_constructible_v<D, F&&>) {
        FixedFunction replacement(std::forward<F>(f)); // reuses the checked constructor
        return *this = std::move(replacement);
    }

    ~FixedFunction() {
        reset();
    }

    /// Destroy the target and become empty.
    void reset() noexcept {
        if (vtable_) {
            vtable_->destroy(storage_);
            vtable_ = nullptr;
        }
    }

    void swap(FixedFunction& other) noexcept {
        FixedFunction tmp(std::move(other));
        other = std::move(*this);
        *this = std::move(tmp);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return vtable_ != nullptr;
    }

    friend bool operator==(const FixedFunction& f, std::nullptr_t) noexcept {
        return !f;
    }
    friend bool operator!=(const FixedFunction& f, std::nullptr_t) noexcept {
        return static_cast<bool>(f);
    }

    /// Call the target. Precondition: not empty (undefined behaviour otherwise).
    R operator()(Args... args) {
        return vtable_->invoke(storage_, std::forward<Args>(args)...);
    }

  private:
    struct VTable {
        R (*invoke)(void* target, Args&&... args);
        void (*relocate)(void* dst, void* src) noexcept; // move-construct into dst, destroy src
        void (*destroy)(void* target) noexcept;
    };

    template <typename D>
    static constexpr VTable vtable_for = {
        [](void* target, Args&&... args) -> R {
            D& callable = *static_cast<D*>(target);
            if constexpr (std::is_void_v<R>) {
                callable(std::forward<Args>(args)...);
            } else {
                return callable(std::forward<Args>(args)...);
            }
        },
        [](void* dst, void* src) noexcept {
            ::new (dst) D(std::move(*static_cast<D*>(src)));
            static_cast<D*>(src)->~D();
        },
        [](void* target) noexcept { static_cast<D*>(target)->~D(); },
    };

    void take(FixedFunction& other) noexcept {
        if (other.vtable_) {
            other.vtable_->relocate(storage_, other.storage_);
            vtable_ = other.vtable_;
            other.vtable_ = nullptr;
        }
    }

    alignas(Alignment) unsigned char storage_[Capacity];
    const VTable* vtable_ = nullptr;
};

} // namespace pulp::runtime
