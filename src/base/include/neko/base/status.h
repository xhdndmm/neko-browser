#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace neko::base {

// ---------------------------------------------------------------------------
// Error model
// ---------------------------------------------------------------------------
enum class ErrorCategory : int {
  kNone = 0,
  kInvalidArgument,
  kOutOfMemory,
  kIo,
  kNetwork,
  kParse,
  kJavascript,
  kSecurity,
  kCancelled,
  kNotImplemented,
  kInternal,
  kUnknown,
};

std::string_view ToString(ErrorCategory category);

// A single, recoverable error.  Prefer the named factories (InvalidArgument(),
// Io(), ...) so call sites read clearly.  See docs/design/errors.md.
class Error {
 public:
  Error() = default;
  Error(ErrorCategory category, std::string message);

  static Error Unknown(std::string message);
  static Error InvalidArgument(std::string message);
  static Error Io(std::string message);
  static Error Network(std::string message);
  static Error Parse(std::string message);
  static Error Javascript(std::string message);
  static Error Security(std::string message);
  static Error Cancelled(std::string message);
  static Error NotImplemented(std::string message);

  ErrorCategory category() const;
  const std::string& message() const;

  // True when the Error is the default (non-error) state.
  bool ok() const;
  explicit operator bool() const;

 private:
  ErrorCategory category_ = ErrorCategory::kNone;
  std::string message_;
};

bool operator==(const Error& lhs, const Error& rhs);
bool operator!=(const Error& lhs, const Error& rhs);

// Thrown when .value() is called on a Result that holds an Error.
class BadResultAccess : public std::runtime_error {
 public:
  explicit BadResultAccess(const Error& error);
  ErrorCategory category() const;

 private:
  ErrorCategory category_;
};

// ---------------------------------------------------------------------------
// Result<T>
// ---------------------------------------------------------------------------
// A value-or-error union.  Prefer over exceptions for expected failures
// (parsing, IO, network) so control flow stays explicit and testable.
//
// Usage:
//   Result<int> Parse(std::string_view s);
//   Result<int> r = Parse("42");
//   if (!r) { LOG(...); return r.error(); }
//   int v = r.value();
template <typename T>
class [[nodiscard]] Result {
 public:
  using ValueType = T;

  // Implicit conversions make `return 42;` / `return Error::Io(...)` natural.
  // NOLINTBEGIN(google-explicit-constructor)
  Result(T value) : state_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : state_(std::in_place_index<1>, std::move(error)) {}
  // NOLINTEND(google-explicit-constructor)

  static Result Ok(T value) { return Result(std::move(value)); }
  static Result Err(Error error) { return Result(std::move(error)); }

  bool has_value() const { return state_.index() == 0; }
  explicit operator bool() const { return has_value(); }

  T& value() & {
    Check();
    return std::get<0>(state_);
  }
  const T& value() const& {
    Check();
    return std::get<0>(state_);
  }
  T&& value() && {
    Check();
    return std::move(std::get<0>(state_));
  }

  const Error& error() const { return std::get<1>(state_); }
  Error& error() { return std::get<1>(state_); }

  T value_or(T fallback) const& {
    return has_value() ? std::get<0>(state_) : std::move(fallback);
  }
  T value_or(T fallback) && {
    return has_value() ? std::move(std::get<0>(state_)) : std::move(fallback);
  }

 private:
  void Check() const {
    if (!has_value()) {
      throw BadResultAccess(std::get<1>(state_));
    }
  }

  std::variant<T, Error> state_;
};

// Specialization for operations that produce no value.
template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}  // NOLINT

  static Result Ok() { return Result(); }
  static Result Err(Error error) { return Result(std::move(error)); }

  bool has_value() const { return error_.ok(); }
  explicit operator bool() const { return has_value(); }
  void value() const { Check(); }

  const Error& error() const { return error_; }
  Error& error() { return error_; }

 private:
  void Check() const {
    if (!has_value()) {
      throw BadResultAccess(error_);
    }
  }

  Error error_;
};

// Status is the conventional name for a valueless Result.
using Status = Result<void>;

// ---------------------------------------------------------------------------
// Convenience constructors so call sites read as `return Ok(42);` /
// `return Err(Error::Io(...));`.
// ---------------------------------------------------------------------------
// ErrorResult is a lightweight carrier: `Err(e)` yields an ErrorResult which
// implicitly converts to any Result<T> (including Result<void>), so it can be
// returned from functions with any value type.
class [[nodiscard]] ErrorResult {
 public:
  explicit ErrorResult(Error error) : error_(std::move(error)) {}  // NOLINT

  template <typename T>
  operator Result<T>() const {
    return Result<T>::Err(error_);
  }

  operator Result<void>() const { return Result<void>::Err(error_); }  // NOLINT

 private:
  Error error_;
};

inline ErrorResult Err(Error error) { return ErrorResult(std::move(error)); }

inline Result<void> Ok() { return Result<void>::Ok(); }

template <typename T>
Result<T> Ok(T value) {
  return Result<T>::Ok(std::move(value));
}

}  // namespace neko::base

// ---------------------------------------------------------------------------
// NEKO_TRY
// ---------------------------------------------------------------------------
// Early-returns the error of a failed Result expression:
//
//   Result<int> Outer() {
//     const int parsed = NEKO_TRY(ParseValue());
//     return parsed + 1;
//   }
//
// Implemented with GNU statement expressions; unavailable on MSVC (a loud
// compile error is produced if used there).  On MSVC use the explicit
// two-step pattern instead:
//   auto result = ParseValue();
//   if (!result.has_value()) { return result.error(); }
//   auto parsed = std::move(result).value();
#if defined(__GNUC__) || defined(__clang__)
// __extension__ silences the -Wpedantic warning for the GNU statement
// expression; GCC and Clang both support it.
#define NEKO_TRY(expr)                                                        \
  __extension__({                                                             \
    auto&& neko_try_result_ = (expr);                                         \
    if (!neko_try_result_.has_value()) {                                      \
      return neko_try_result_.error();                                        \
    }                                                                         \
    std::forward<decltype(neko_try_result_)>(neko_try_result_).value();       \
  })
#else
#define NEKO_TRY(expr) \
  static_assert(false, "NEKO_TRY requires GNU statement expressions (GCC/Clang)")
#endif
