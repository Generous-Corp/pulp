#pragma once

/// @file result.hpp
/// `Result<T, E>` — success / failure union for fallible operations.
///
/// Closes the gap-doc Runtime row "Result" (currently `std::optional`
/// throughout — drops the error context). This header is a
/// dependency-free placeholder until `std::expected` (C++23) is
/// available across all target toolchains. The shape is intentionally
/// API-compatible with `std::expected` so callers can migrate
/// mechanically (`Ok` / `Err` map to `T` / `unexpected<E>`).
///
/// Why not just adopt `std::expected` today? Pulp's stable toolchain
/// matrix still includes platforms whose stdlib lacks `<expected>` (or
/// has it only behind a feature flag); shipping `pulp::runtime::Result`
/// gives the codebase a single typed return surface today without
/// blocking on the per-platform `<expected>` rollout.
///
/// Headless-friendly. Header-only. No allocations beyond the cost of
/// constructing `T` or `E`. RT-callable as long as `T` and `E` are.

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace pulp::runtime {

template <typename E>
class Err;

namespace detail {

template <typename T>
struct OkTag {};
template <typename E>
struct ErrTag {};

}  // namespace detail

/// Wrap a value as a success result. Use the deduction-guide form:
/// `return Result<int, std::string>(Ok(42));`
template <typename T>
class Ok {
public:
    explicit Ok(T value) : value_(std::move(value)) {}
    T&       value() &       { return value_; }
    const T& value() const & { return value_; }
    T&&      value() &&      { return std::move(value_); }
private:
    T value_;
};

template <typename T>
Ok(T) -> Ok<T>;

/// Wrap a value as a failure result.
/// `return Result<int, std::string>(Err(std::string("bad")));`
template <typename E>
class Err {
public:
    explicit Err(E value) : value_(std::move(value)) {}
    E&       value() &       { return value_; }
    const E& value() const & { return value_; }
    E&&      value() &&      { return std::move(value_); }
private:
    E value_;
};

template <typename E>
Err(E) -> Err<E>;

/// Result<T, E> — either holds a T or an E, never both.
///
/// Inspired by Rust's `Result` and `std::expected`. `T` and `E` must
/// be distinct types (avoids construct-from-ambiguity).
template <typename T, typename E>
class Result {
    static_assert(!std::is_same_v<T, E>, "Result<T,E> requires distinct T and E");

public:
    using value_type = T;
    using error_type = E;

    // Default-constructed Result is a value-initialised T (matches
    // std::expected's default behaviour when T is default-constructible).
    Result()
        requires std::is_default_constructible_v<T>
        : has_value_(true) {
        new (&storage_.value) T();
    }

    Result(Ok<T> ok)
        : has_value_(true) {
        new (&storage_.value) T(std::move(ok).value());
    }

    Result(Err<E> err)
        : has_value_(false) {
        new (&storage_.error) E(std::move(err).value());
    }

    Result(const Result& other) : has_value_(other.has_value_) {
        if (has_value_) new (&storage_.value) T(other.storage_.value);
        else            new (&storage_.error) E(other.storage_.error);
    }

    Result(Result&& other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                    std::is_nothrow_move_constructible_v<E>)
        : has_value_(other.has_value_) {
        if (has_value_) new (&storage_.value) T(std::move(other.storage_.value));
        else            new (&storage_.error) E(std::move(other.storage_.error));
    }

    // Strong exception guarantee. If T's or E's copy/move ctor throws,
    // the destination is left holding its previous value — the old
    // member is only destroyed AFTER the new one is fully constructed.
    // Pinned by the Codex P2 review comment "Preserve Result object
    // when assignment construction throws".
    Result& operator=(const Result& other) {
        if (this != &other) {
            if (other.has_value_) {
                T tmp(other.storage_.value);   // may throw, *this untouched
                destroy();
                has_value_ = true;
                new (&storage_.value) T(std::move(tmp));
            } else {
                E tmp(other.storage_.error);
                destroy();
                has_value_ = false;
                new (&storage_.error) E(std::move(tmp));
            }
        }
        return *this;
    }

    Result& operator=(Result&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> &&
        std::is_nothrow_move_constructible_v<E>) {
        if (this != &other) {
            if (other.has_value_) {
                T tmp(std::move(other.storage_.value));
                destroy();
                has_value_ = true;
                new (&storage_.value) T(std::move(tmp));
            } else {
                E tmp(std::move(other.storage_.error));
                destroy();
                has_value_ = false;
                new (&storage_.error) E(std::move(tmp));
            }
        }
        return *this;
    }

    ~Result() { destroy(); }

    // ── Inspection ──────────────────────────────────────────────────────

    bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }
    bool is_err() const noexcept { return !has_value_; }

    // ── Value access ────────────────────────────────────────────────────
    //
    // Pre: `has_value()`. Calling these on an error result is UB — the
    // caller is expected to check `has_value()` / `operator bool()`
    // first, matching `std::expected`'s `operator*` contract.

    T&       value() &       { return storage_.value; }
    const T& value() const & { return storage_.value; }
    T&&      value() &&      { return std::move(storage_.value); }

    T&       operator*() &       { return storage_.value; }
    const T& operator*() const & { return storage_.value; }
    T*       operator->()        { return &storage_.value; }
    const T* operator->() const  { return &storage_.value; }

    /// Return the held value, or `fallback` when this is an error.
    template <typename U>
    T value_or(U&& fallback) const & {
        return has_value_ ? storage_.value
                          : static_cast<T>(std::forward<U>(fallback));
    }

    // ── Error access ────────────────────────────────────────────────────
    //
    // Pre: `!has_value()`.

    E&       error() &       { return storage_.error; }
    const E& error() const & { return storage_.error; }
    E&&      error() &&      { return std::move(storage_.error); }

private:
    union Storage {
        Storage() {}
        ~Storage() {}
        T value;
        E error;
    } storage_;
    bool has_value_;

    void destroy() noexcept {
        if (has_value_) storage_.value.~T();
        else            storage_.error.~E();
    }
};

}  // namespace pulp::runtime
