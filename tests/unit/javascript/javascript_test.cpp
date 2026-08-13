// Unit tests for the neko::javascript module (QuickJS runtime wrapper).

#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "neko/base/status.h"
#include "neko/javascript/script_engine.h"

namespace neko::javascript {
namespace {

class ScriptEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    engine_ = std::make_unique<ScriptEngine>();
    engine_->SetConsoleSink([this](std::string_view level, std::string_view text) {
      console_.push_back(std::string(level) + ": " + std::string(text));
    });
  }

  std::unique_ptr<ScriptEngine> engine_;
  std::vector<std::string> console_;
};

TEST_F(ScriptEngineTest, RuntimeInfo) {
  EXPECT_EQ(ScriptEngine::RuntimeName(), "QuickJS");
  EXPECT_FALSE(ScriptEngine::Version().empty());
}

TEST_F(ScriptEngineTest, EvaluatesArithmetic) {
  auto result = engine_->Evaluate("1 + 2 * 3");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(result.value().Kind(), ValueKind::kNumber);
  auto num = result.value().ToNumber();
  ASSERT_TRUE(num.has_value());
  EXPECT_DOUBLE_EQ(num.value(), 7.0);
}

TEST_F(ScriptEngineTest, EvaluatesString) {
  auto result = engine_->Evaluate("'hello ' + 'world'");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().Kind(), ValueKind::kString);
  auto str = result.value().ToString();
  ASSERT_TRUE(str.has_value());
  EXPECT_EQ(str.value(), "hello world");
}

TEST_F(ScriptEngineTest, EvaluatesObjectAndJson) {
  auto result = engine_->Evaluate("({a: 1, b: 'x'})");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().Kind(), ValueKind::kObject);
  auto json = result.value().JsonStringify();
  ASSERT_TRUE(json.has_value()) << json.error().message();
  EXPECT_EQ(json.value(), R"({"a":1,"b":"x"})");
}

TEST_F(ScriptEngineTest, EvaluatesBooleanAndNull) {
  auto t = engine_->Evaluate("true");
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t.value().Kind(), ValueKind::kBoolean);
  auto tb = t.value().ToBoolean();
  ASSERT_TRUE(tb.has_value());
  EXPECT_TRUE(tb.value());

  auto n = engine_->Evaluate("null");
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(n.value().Kind(), ValueKind::kNull);
}

TEST_F(ScriptEngineTest, ToNumberConversion) {
  auto r = engine_->Evaluate("'42'");
  ASSERT_TRUE(r.has_value());
  auto num = r.value().ToNumber();
  ASSERT_TRUE(num.has_value());
  EXPECT_DOUBLE_EQ(num.value(), 42.0);
}

TEST_F(ScriptEngineTest, DefineFunctionAndCallGlobal) {
  auto define = engine_->Evaluate("function add(a, b) { return a + b; }");
  ASSERT_TRUE(define.has_value());
  EXPECT_EQ(define.value().Kind(), ValueKind::kUndefined);

  auto n2 = engine_->MakeNumber(2);
  auto n3 = engine_->MakeNumber(3);
  ASSERT_TRUE(n2.has_value());
  ASSERT_TRUE(n3.has_value());
  auto result = engine_->CallGlobal("add", {n2.value(), n3.value()});
  ASSERT_TRUE(result.has_value()) << result.error().message();
  auto num = result.value().ToNumber();
  ASSERT_TRUE(num.has_value());
  EXPECT_DOUBLE_EQ(num.value(), 5.0);
}

TEST_F(ScriptEngineTest, CallGlobalOnMissingFunction) {
  auto result = engine_->CallGlobal("doesNotExist");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kJavascript);
}

TEST_F(ScriptEngineTest, SetAndGetGlobal) {
  auto s = engine_->MakeString("neko");
  ASSERT_TRUE(s.has_value());
  auto set = engine_->SetGlobal("project", s.value());
  ASSERT_TRUE(set.has_value()) << set.error().message();

  auto get = engine_->GetGlobal("project");
  ASSERT_TRUE(get.has_value());
  EXPECT_EQ(get.value().Kind(), ValueKind::kString);
  auto str = get.value().ToString();
  ASSERT_TRUE(str.has_value());
  EXPECT_EQ(str.value(), "neko");
}

TEST_F(ScriptEngineTest, PersistentGlobalStateAcrossEvaluations) {
  ASSERT_TRUE(engine_->Evaluate("counter = 1;").has_value());
  auto bump = engine_->Evaluate("counter += 41; counter;");
  ASSERT_TRUE(bump.has_value());
  auto num = bump.value().ToNumber();
  ASSERT_TRUE(num.has_value());
  EXPECT_DOUBLE_EQ(num.value(), 42.0);
}

TEST_F(ScriptEngineTest, SyntaxErrorIsParseError) {
  auto result = engine_->Evaluate("function (");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kParse);
  EXPECT_NE(result.error().message().find("SyntaxError"), std::string::npos);
}

TEST_F(ScriptEngineTest, RuntimeErrorIsJavascriptError) {
  auto result = engine_->Evaluate("throw new Error('boom')");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kJavascript);
  EXPECT_NE(result.error().message().find("boom"), std::string::npos);
}

TEST_F(ScriptEngineTest, ConsoleLogCapture) {
  auto result = engine_->Evaluate("console.log('hello', 42, {k: 1})");
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_EQ(console_.size(), 1u);
  EXPECT_EQ(console_[0], "log: hello 42 {\"k\":1}");
}

TEST_F(ScriptEngineTest, ConsoleErrorCapture) {
  (void)engine_->Evaluate("console.error('fatal'); console.warn('careful')");
  ASSERT_EQ(console_.size(), 2u);
  EXPECT_EQ(console_[0], "error: fatal");
  EXPECT_EQ(console_[1], "warning: careful");
}

TEST_F(ScriptEngineTest, ConsoleSinkCanBeChanged) {
  std::vector<std::string> second;
  engine_->SetConsoleSink([&second](std::string_view, std::string_view text) {
    second.push_back(std::string(text));
  });
  (void)engine_->Evaluate("console.log('moved')");
  ASSERT_EQ(console_.size(), 0u);
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(second[0], "moved");
}

TEST_F(ScriptEngineTest, InfiniteLoopIsInterrupted) {
  engine_->SetExecutionLimit(std::chrono::milliseconds(50));
  const auto start = std::chrono::steady_clock::now();
  auto result = engine_->Evaluate("while (true) {}");
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kJavascript);
  EXPECT_NE(result.error().message().find("interrupted"), std::string::npos);
  EXPECT_LT(elapsed, std::chrono::seconds(10));
}

TEST_F(ScriptEngineTest, CallGlobalInfiniteLoopIsInterrupted) {
  ASSERT_TRUE(engine_->Evaluate("function spin() { while (true) {} }").has_value());
  engine_->SetExecutionLimit(std::chrono::milliseconds(50));
  auto result = engine_->CallGlobal("spin");
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message().find("interrupted"), std::string::npos);
}

TEST_F(ScriptEngineTest, MemoryLimitIsEnforced) {
  engine_->SetMemoryLimit(1024u * 1024u);  // 1 MiB
  // A large allocation must be rejected by the runtime.  (new Array(n) only
  // sets length and does not actually allocate, so allocate a big string.)
  auto result = engine_->Evaluate("'x'.repeat(8 * 1024 * 1024);");
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kJavascript);
}

TEST_F(ScriptEngineTest, ValueOutlivesEngine) {
  ScriptValue saved;
  {
    ScriptEngine temp;
    auto result = temp.Evaluate("'survivor'");
    ASSERT_TRUE(result.has_value());
    saved = result.value();
  }
  // The value keeps the runtime alive even after the engine is destroyed.
  ASSERT_TRUE(saved.IsValid());
  EXPECT_EQ(saved.Kind(), ValueKind::kString);
  auto str = saved.ToString();
  ASSERT_TRUE(str.has_value());
  EXPECT_EQ(str.value(), "survivor");
}

TEST_F(ScriptEngineTest, ValueCopyKeepsReference) {
  auto result = engine_->Evaluate("({n: 7})");
  ASSERT_TRUE(result.has_value());
  ScriptValue copy = result.value();
  ASSERT_TRUE(copy.IsValid());
  auto json = copy.JsonStringify();
  ASSERT_TRUE(json.has_value());
  EXPECT_EQ(json.value(), R"({"n":7})");
}

TEST_F(ScriptEngineTest, ValueMoveTransfersOwnership) {
  auto result = engine_->Evaluate("'moved'");
  ASSERT_TRUE(result.has_value());
  ScriptValue moved = std::move(result.value());
  EXPECT_FALSE(result.value().IsValid());
  ASSERT_TRUE(moved.IsValid());
  auto str = moved.ToString();
  ASSERT_TRUE(str.has_value());
  EXPECT_EQ(str.value(), "moved");
}

TEST_F(ScriptEngineTest, DefaultValueIsInvalid) {
  ScriptValue v;
  EXPECT_FALSE(v.IsValid());
  EXPECT_EQ(v.Kind(), ValueKind::kInvalid);
  auto str = v.ToString();
  ASSERT_FALSE(str.has_value());
  EXPECT_EQ(str.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST_F(ScriptEngineTest, BigIntKind) {
  auto result = engine_->Evaluate("123n");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().Kind(), ValueKind::kBigInt);
}

TEST_F(ScriptEngineTest, FunctionKind) {
  auto result = engine_->Evaluate("function f() {}");
  ASSERT_TRUE(result.has_value());
  // A function declaration evaluates to undefined; grab it from the global.
  auto f = engine_->GetGlobal("f");
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f.value().Kind(), ValueKind::kFunction);
}

TEST_F(ScriptEngineTest, ValueKindToString) {
  EXPECT_EQ(ToString(ValueKind::kNumber), "number");
  EXPECT_EQ(ToString(ValueKind::kString), "string");
  EXPECT_EQ(ToString(ValueKind::kObject), "object");
  EXPECT_EQ(ToString(ValueKind::kFunction), "function");
  EXPECT_EQ(ToString(ValueKind::kInvalid), "invalid");
}

TEST_F(ScriptEngineTest, ArgumentsFromDifferentEngineRejected) {
  ScriptEngine other;
  auto other_val = other.MakeNumber(1);
  ASSERT_TRUE(other_val.has_value());
  auto n = engine_->MakeNumber(2);
  ASSERT_TRUE(n.has_value());
  ASSERT_TRUE(engine_->Evaluate("function f(x) { return x; }").has_value());
  auto result = engine_->CallGlobal("f", {other_val.value()});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().category(), base::ErrorCategory::kInvalidArgument);
}

TEST_F(ScriptEngineTest, JsonStringifyFailureOnCycle) {
  auto result = engine_->Evaluate("var o = {}; o.self = o; o;");
  ASSERT_TRUE(result.has_value());
  auto json = result.value().JsonStringify();
  ASSERT_FALSE(json.has_value());
  EXPECT_EQ(json.error().category(), base::ErrorCategory::kJavascript);
}

}  // namespace
}  // namespace neko::javascript
