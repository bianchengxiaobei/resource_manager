#pragma once

#include "parcel/core/config.hpp"
#include "parcel/core/error.hpp"

#include <optional>
#include <type_traits>
#include <utility>

namespace parcel::core {

template <typename T> class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>, "Result<T> cannot hold a reference");
    static_assert(!std::is_void_v<T>, "Use Result<void> for empty results");

  public:
    using ValueType = T;

    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : error_(std::move(error)) {}

    Result(Result&& other) noexcept(std::is_nothrow_move_constructible_v<T>) = default;
    Result& operator=(Result&& other) noexcept(std::is_nothrow_move_assignable_v<T>) = default;

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    [[nodiscard]] bool HasValue() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return HasValue(); }

    T& Value() & {
        EnsureHasValue();
        return *value_;
    }

    const T& Value() const& {
        EnsureHasValue();
        return *value_;
    }

    T&& Value() && {
        EnsureHasValue();
        return std::move(*value_);
    }

    T ValueOr(T fallback) && {
        if (!value_) {
            return fallback;
        }
        return std::move(*value_);
    }

    Error& GetError() & {
        EnsureHasError();
        return error_;
    }

    const Error& GetError() const& {
        EnsureHasError();
        return error_;
    }

    Error&& TakeError() && {
        EnsureHasError();
        return std::move(error_);
    }

  private:
    void EnsureHasValue() const {
        if (!value_) {
            PARCEL_VERIFY(false);
        }
    }

    void EnsureHasError() const {
        if (value_) {
            PARCEL_VERIFY(false);
        }
    }

    std::optional<T> value_;
    Error error_;
};

template <> class [[nodiscard]] Result<void> {
  public:
    Result() noexcept = default;
    Result(Error error) : error_(std::move(error)), ok_(false) {}

    Result(Result&&) noexcept = default;
    Result& operator=(Result&&) noexcept = default;

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    [[nodiscard]] bool HasValue() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    void Value() const { EnsureHasValue(); }

    Error& GetError() & {
        EnsureHasError();
        return error_;
    }

    const Error& GetError() const& {
        EnsureHasError();
        return error_;
    }

    Error&& TakeError() && {
        EnsureHasError();
        return std::move(error_);
    }

  private:
    void EnsureHasValue() const {
        if (!ok_) {
            PARCEL_VERIFY(false);
        }
    }

    void EnsureHasError() const {
        if (ok_) {
            PARCEL_VERIFY(false);
        }
    }

    Error error_;
    bool ok_ = true;
};

namespace detail {

[[nodiscard]] inline Result<void> Ok() noexcept { return {}; }

} // namespace detail

} // namespace parcel::core

#define PARCEL_TRY(expression)                                                                     \
    do {                                                                                           \
        auto parcel_try_result_ = (expression);                                                    \
        if (!parcel_try_result_) {                                                                 \
            return std::move(parcel_try_result_).TakeError();                                      \
        }                                                                                          \
    } while (false)

#define PARCEL_TRY_ASSIGN(target, expression)                                                      \
    do {                                                                                           \
        auto parcel_try_result_ = (expression);                                                    \
        if (!parcel_try_result_) {                                                                 \
            return std::move(parcel_try_result_).TakeError();                                      \
        }                                                                                          \
        (target) = std::move(parcel_try_result_).Value();                                          \
    } while (false)

#define PARCEL_TRY_DECL(type, name, expression)                                                    \
    type name{};                                                                                   \
    PARCEL_TRY_ASSIGN(name, expression)
