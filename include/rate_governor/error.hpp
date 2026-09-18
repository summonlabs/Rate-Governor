#ifndef RATE_GOVERNOR_ERROR_HPP
#define RATE_GOVERNOR_ERROR_HPP

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "rate_governor/checked_math.hpp"
#include "rate_governor/reason.hpp"

namespace rate_governor {

enum class ErrorCode : std::uint16_t {
  Ok = 0,
  InvalidArgument,
  NotFound,
  Conflict,
  StaleAuthority,
  Fenced,
  LimitExceeded,
  JournalFailure,
  BackendFailure,
  Unsupported,
  ShuttingDown,
  Denied,
  Unknown,
};

[[nodiscard]] std::string_view to_string_view(ErrorCode code) noexcept;

struct Error {
  ErrorCode code{ErrorCode::Unknown};
  ReasonCode reason{ReasonCode::None};
  std::string message;

  [[nodiscard]] std::string to_string() const;
};

[[nodiscard]] inline Error make_error(ErrorCode code, ReasonCode reason, std::string message) {
  Error error;
  error.code = code;
  error.reason = reason;
  error.message = std::move(message);
  return error;
}

class Status {
 public:
  Status() = default;
  explicit Status(Error error) : error_(std::move(error)) {}
  [[nodiscard]] static Status ok() { return Status(); }
  [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] const Error& error() const noexcept { return error_.value(); }
  [[nodiscard]] ErrorCode code() const noexcept {
    return error_.has_value() ? error_->code : ErrorCode::Ok;
  }
  [[nodiscard]] ReasonCode reason() const noexcept {
    return error_.has_value() ? error_->reason : ReasonCode::None;
  }
  [[nodiscard]] std::string message() const {
    return error_.has_value() ? error_->message : std::string("ok");
  }

 private:
  std::optional<Error> error_;
};

template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] T& value() noexcept { return value_.value(); }
  [[nodiscard]] const T& value() const noexcept { return value_.value(); }
  [[nodiscard]] T&& move_value() noexcept { return std::move(value_.value()); }
  [[nodiscard]] const Error& error() const noexcept { return error_.value(); }
  [[nodiscard]] ErrorCode code() const noexcept {
    return error_.has_value() ? error_->code : ErrorCode::Ok;
  }
  [[nodiscard]] ReasonCode reason() const noexcept {
    return error_.has_value() ? error_->reason : ReasonCode::None;
  }
  [[nodiscard]] std::string message() const {
    return error_.has_value() ? error_->message : std::string("ok");
  }

 private:
  std::optional<T> value_;
  std::optional<Error> error_;
};

}  // namespace rate_governor

#endif  // RATE_GOVERNOR_ERROR_HPP
