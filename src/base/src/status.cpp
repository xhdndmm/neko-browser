#include "neko/base/status.h"

#include <string>
#include <string_view>
#include <utility>

namespace neko::base {

std::string_view ToString(ErrorCategory category)
{
  switch (category) {
  case ErrorCategory::kNone:
    return "none";
  case ErrorCategory::kInvalidArgument:
    return "invalid_argument";
  case ErrorCategory::kOutOfMemory:
    return "out_of_memory";
  case ErrorCategory::kIo:
    return "io";
  case ErrorCategory::kNetwork:
    return "network";
  case ErrorCategory::kParse:
    return "parse";
  case ErrorCategory::kJavascript:
    return "javascript";
  case ErrorCategory::kSecurity:
    return "security";
  case ErrorCategory::kCancelled:
    return "cancelled";
  case ErrorCategory::kNotImplemented:
    return "not_implemented";
  case ErrorCategory::kInternal:
    return "internal";
  case ErrorCategory::kUnknown:
    return "unknown";
  }
  return "unknown";
}

Error::Error(ErrorCategory category, std::string message)
    : category_(category), message_(std::move(message))
{}

Error Error::Unknown(std::string message)
{
  return Error(ErrorCategory::kUnknown, std::move(message));
}

Error Error::InvalidArgument(std::string message)
{
  return Error(ErrorCategory::kInvalidArgument, std::move(message));
}

Error Error::Io(std::string message)
{
  return Error(ErrorCategory::kIo, std::move(message));
}

Error Error::Network(std::string message)
{
  return Error(ErrorCategory::kNetwork, std::move(message));
}

Error Error::Parse(std::string message)
{
  return Error(ErrorCategory::kParse, std::move(message));
}

Error Error::Javascript(std::string message)
{
  return Error(ErrorCategory::kJavascript, std::move(message));
}

Error Error::Security(std::string message)
{
  return Error(ErrorCategory::kSecurity, std::move(message));
}

Error Error::Cancelled(std::string message)
{
  return Error(ErrorCategory::kCancelled, std::move(message));
}

Error Error::NotImplemented(std::string message)
{
  return Error(ErrorCategory::kNotImplemented, std::move(message));
}

ErrorCategory Error::category() const
{
  return category_;
}
const std::string& Error::message() const
{
  return message_;
}
bool Error::ok() const
{
  return category_ == ErrorCategory::kNone;
}
Error::operator bool() const
{
  return category_ != ErrorCategory::kNone;
}

bool operator==(const Error& lhs, const Error& rhs)
{
  return lhs.category() == rhs.category() && lhs.message() == rhs.message();
}

bool operator!=(const Error& lhs, const Error& rhs)
{
  return !(lhs == rhs);
}

BadResultAccess::BadResultAccess(const Error& error)
    : std::runtime_error("bad result access: [" + std::string(ToString(error.category())) + "] " +
                         error.message()),
      category_(error.category())
{}

ErrorCategory BadResultAccess::category() const
{
  return category_;
}

} // namespace neko::base
