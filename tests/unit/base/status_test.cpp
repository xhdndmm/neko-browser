#include "neko/base/status.h"

#include <string>

#include <gtest/gtest.h>

namespace neko::base {
namespace {

TEST(StatusTest, OkResultHasValue) {
  const Result<int> r = Ok(42);
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(r);
  EXPECT_EQ(r.value(), 42);
}

TEST(StatusTest, ErrorResultHasNoValue) {
  const Result<int> r = Err(Error::InvalidArgument("bad input"));
  ASSERT_FALSE(r.has_value());
  EXPECT_FALSE(r);
  EXPECT_EQ(r.error().category(), ErrorCategory::kInvalidArgument);
  EXPECT_EQ(r.error().message(), "bad input");
}

TEST(StatusTest, ImplicitValueAndErrorConversion) {
  const auto f = [](bool fail) -> Result<std::string> {
    if (fail) {
      return Error::Io("disk gone");
    }
    return std::string("hello");
  };
  ASSERT_TRUE(f(false).has_value());
  EXPECT_EQ(f(false).value(), "hello");
  ASSERT_FALSE(f(true).has_value());
  EXPECT_EQ(f(true).error().category(), ErrorCategory::kIo);
}

TEST(StatusTest, VoidResult) {
  const Status ok = Ok();
  ASSERT_TRUE(ok.has_value());
  EXPECT_NO_THROW(ok.value());

  const Status err = Err(Error::Network("timeout"));
  ASSERT_FALSE(err.has_value());
  EXPECT_EQ(err.error().category(), ErrorCategory::kNetwork);
}

TEST(StatusTest, ValueOrFallsBack) {
  const Result<int> ok = Ok(7);
  EXPECT_EQ(ok.value_or(-1), 7);
  const Result<int> err = Err(Error::Unknown("boom"));
  EXPECT_EQ(err.value_or(-1), -1);
}

TEST(StatusTest, BadResultAccessThrows) {
  const Result<int> err = Err(Error::Parse("bad token"));
  EXPECT_THROW(static_cast<void>(err.value()), BadResultAccess);
}

TEST(StatusTest, ErrorFactories) {
  EXPECT_EQ(Error::NotImplemented("x").category(), ErrorCategory::kNotImplemented);
  EXPECT_EQ(Error::Security("x").category(), ErrorCategory::kSecurity);
  EXPECT_EQ(Error::Cancelled("x").category(), ErrorCategory::kCancelled);
  EXPECT_EQ(Error::Parse("x").category(), ErrorCategory::kParse);
  EXPECT_EQ(Error::Javascript("x").category(), ErrorCategory::kJavascript);
  EXPECT_EQ(Error::Io("x").category(), ErrorCategory::kIo);
  EXPECT_EQ(Error::Network("x").category(), ErrorCategory::kNetwork);
  EXPECT_EQ(Error::Unknown("x").category(), ErrorCategory::kUnknown);
  EXPECT_EQ(Error::InvalidArgument("x").category(), ErrorCategory::kInvalidArgument);
}

TEST(StatusTest, DefaultErrorIsOk) {
  const Error e;
  EXPECT_TRUE(e.ok());
  EXPECT_FALSE(e);
  EXPECT_EQ(e.category(), ErrorCategory::kNone);
}

TEST(StatusTest, ErrorEquality) {
  EXPECT_EQ(Error::Io("same"), Error::Io("same"));
  EXPECT_NE(Error::Io("same"), Error::Io("different"));
  EXPECT_NE(Error::Io("same"), Error::Parse("same"));
}

TEST(StatusTest, ErrorCategoryToString) {
  EXPECT_EQ(ToString(ErrorCategory::kIo), "io");
  EXPECT_EQ(ToString(ErrorCategory::kNotImplemented), "not_implemented");
  EXPECT_EQ(ToString(ErrorCategory::kJavascript), "javascript");
  EXPECT_EQ(ToString(ErrorCategory::kSecurity), "security");
}

TEST(StatusTest, NekoTryEarlyReturns) {
#if defined(__GNUC__) || defined(__clang__)
  const auto inner = []() -> Result<int> { return Err(Error::Unknown("inner fail")); };
  const auto outer = [&]() -> Result<int> {
    const int v = NEKO_TRY(inner());
    return v + 1;
  };

  const Result<int> res = outer();
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().message(), "inner fail");
  EXPECT_EQ(res.error().category(), ErrorCategory::kUnknown);
#endif
}

TEST(StatusTest, ResultSupportsMoveOnlyValues) {
  struct MoveOnly {
    explicit MoveOnly(int v) : value(v) {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
    int value;
  };

  const auto f = []() -> Result<MoveOnly> { return MoveOnly(5); };
  Result<MoveOnly> r = f();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value().value, 5);
}

}  // namespace
}  // namespace neko::base
