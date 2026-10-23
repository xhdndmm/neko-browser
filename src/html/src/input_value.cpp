#include "neko/html/input_value.h"

#include "neko/base/string_util.h"
#include "neko/css/color.h"
#include "neko/dom/element.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace neko::html {
namespace {

// ASCII whitespace as HTML defines it (space, tab, LF, FF, CR); note that this
// deliberately excludes U+000B, unlike the engine's generic base::Trim.
bool IsAsciiWhitespace(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

std::string_view TrimAsciiWhitespace(std::string_view text)
{
  while (!text.empty() && IsAsciiWhitespace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsAsciiWhitespace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// "Strip newlines from the value" (WHATWG HTML §4.10.5.1, Text/Search/...,
// Password, Hidden, URL and Email states): remove every U+000A and U+000D.
std::string StripNewlines(std::string_view value)
{
  std::string result;
  result.reserve(value.size());
  for (const char c : value) {
    if (c != '\n' && c != '\r') {
      result.push_back(c);
    }
  }
  return result;
}

std::string RawAttr(const dom::Element& element, std::string_view name)
{
  return std::string(element.GetAttribute(name).value_or(""));
}

// Is |text| a valid floating-point number per HTML §2.3.4?  This is stricter
// than std::strtod: no leading "+", no surrounding whitespace, at least one
// digit in the mantissa, and a complete exponent when one is present.
bool IsValidFloatingPointSyntax(std::string_view text)
{
  std::size_t i = 0;
  if (i < text.size() && text[i] == '-') {
    ++i;
  }
  std::size_t integer_digits = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    ++i;
    ++integer_digits;
  }
  std::size_t fraction_digits = 0;
  if (i < text.size() && text[i] == '.') {
    ++i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      ++i;
      ++fraction_digits;
    }
    if (fraction_digits == 0) {
      return false; // "1." is not a valid floating-point number
    }
  }
  if (integer_digits == 0 && fraction_digits == 0) {
    return false;
  }
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
      ++i;
    }
    std::size_t exponent_digits = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      ++i;
      ++exponent_digits;
    }
    if (exponent_digits == 0) {
      return false;
    }
  }
  return i == text.size();
}

// The numeric value of a valid floating-point number.  Overflow (a non-finite
// result) is an error; underflow is not, and yields (a representation of) 0.
std::optional<double> ParseFloatingPoint(std::string_view text)
{
  if (!IsValidFloatingPointSyntax(text)) {
    return std::nullopt;
  }
  const std::string copy(text);
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (end != copy.c_str() + copy.size() || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

// "Best representation, as a floating-point number" (HTML §2.3.4): the
// shortest string that round-trips, which is what std::to_chars produces.
std::string FormatFloatingPoint(double value)
{
  if (value == 0) {
    value = 0; // normalise -0
  }
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc()) {
    return std::string();
  }
  return std::string(buffer, result.ptr);
}

bool IsLeapYear(long long year)
{
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(long long year, int month)
{
  static constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && IsLeapYear(year)) {
    return 29;
  }
  return kDays[static_cast<std::size_t>(month - 1)];
}

// Days since 1970-01-01 (Howard Hinnant's civil-from-days inverse).  Used only
// to decide how many ISO weeks a year has, so a signed 64-bit result is ample.
long long DaysFromCivil(long long year, int month, int day)
{
  year -= month <= 2 ? 1 : 0;
  const long long era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy =
      static_cast<unsigned>((153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long long>(doe) - 719468;
}

// 0 = Monday ... 6 = Sunday.
int WeekdayMondayFirst(long long year, int month, int day)
{
  const long long days = DaysFromCivil(year, month, day);
  long long weekday = (days + 3) % 7; // 1970-01-01 was a Thursday (Monday = 0)
  if (weekday < 0) {
    weekday += 7;
  }
  return static_cast<int>(weekday);
}

int IsoWeeksInYear(long long year)
{
  const int jan1 = WeekdayMondayFirst(year, 1, 1);
  return (jan1 == 3 || (jan1 == 2 && IsLeapYear(year))) ? 53 : 52;
}

bool ParseDigits(std::string_view text, int& value)
{
  if (text.empty()) {
    return false;
  }
  value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  return true;
}

// Number of fractional decimal digits in |text| ("0.100" -> 3, "1e-3" -> 3,
// "3" -> 0).  Used to serialize a step-aligned range value without exposing
// binary floating-point noise ("0.30000000000000004" instead of "0.3").
int DecimalPlaces(std::string_view text)
{
  const std::size_t e = text.find_first_of("eE");
  if (e != std::string_view::npos) {
    std::string_view exponent_text = text.substr(e + 1);
    bool negative = false;
    if (!exponent_text.empty() && (exponent_text.front() == '+' || exponent_text.front() == '-')) {
      negative = exponent_text.front() == '-';
      exponent_text.remove_prefix(1);
    }
    int exponent = 0;
    if (!ParseDigits(exponent_text, exponent)) {
      return 0;
    }
    int mantissa_places = 0;
    const std::string_view mantissa = text.substr(0, e);
    const std::size_t dot = mantissa.find('.');
    if (dot != std::string_view::npos) {
      mantissa_places = static_cast<int>(mantissa.size() - dot - 1);
    }
    const int places = negative ? mantissa_places + exponent : mantissa_places - exponent;
    return std::clamp(places, 0, 12);
  }
  const std::size_t dot = text.find('.');
  if (dot == std::string_view::npos) {
    return 0;
  }
  return std::min(static_cast<int>(text.size() - dot - 1), 12);
}

double RoundToDecimalPlaces(double value, int places)
{
  if (places <= 0) {
    return std::round(value);
  }
  double scale = 1.0;
  for (int i = 0; i < places; ++i) {
    scale *= 10.0;
  }
  return std::round(value * scale) / scale;
}

struct DateParts
{
  long long year = 0;
  int month = 0;
  int day = 0;
};

struct TimeParts
{
  int hour = 0;
  int minute = 0;
  int second = 0;
  bool has_seconds = false;
  std::string_view fraction; // digits after the '.', empty when absent
};

bool ParseDate(std::string_view text, DateParts& out)
{
  const std::size_t first_dash = text.find('-');
  if (first_dash == std::string_view::npos || first_dash < 4) {
    return false;
  }
  const std::string_view year_text = text.substr(0, first_dash);
  const std::size_t second_dash = text.find('-', first_dash + 1);
  if (second_dash == std::string_view::npos) {
    return false;
  }
  const std::string_view month_text = text.substr(first_dash + 1, second_dash - first_dash - 1);
  const std::string_view day_text = text.substr(second_dash + 1);
  if (month_text.size() != 2 || day_text.size() != 2) {
    return false;
  }
  long long year = 0;
  for (const char c : year_text) {
    if (c < '0' || c > '9') {
      return false;
    }
    year = year * 10 + (c - '0');
    if (year > 999999999) {
      return false; // HTML only requires "greater than 0"; bound it for date math
    }
  }
  int month = 0;
  int day = 0;
  if (!ParseDigits(month_text, month) || !ParseDigits(day_text, day)) {
    return false;
  }
  if (year <= 0 || month < 1 || month > 12 || day < 1 || day > DaysInMonth(year, month)) {
    return false;
  }
  out = DateParts{year, month, day};
  return true;
}

bool ParseMonth(std::string_view text, long long& year, int& month)
{
  const std::size_t dash = text.find('-');
  if (dash == std::string_view::npos || dash < 4) {
    return false;
  }
  const std::string_view year_text = text.substr(0, dash);
  const std::string_view month_text = text.substr(dash + 1);
  if (month_text.size() != 2) {
    return false;
  }
  year = 0;
  for (const char c : year_text) {
    if (c < '0' || c > '9') {
      return false;
    }
    year = year * 10 + (c - '0');
    if (year > 999999999) {
      return false;
    }
  }
  if (!ParseDigits(month_text, month)) {
    return false;
  }
  return year > 0 && month >= 1 && month <= 12;
}

bool ParseWeek(std::string_view text, long long& year, int& week)
{
  const std::size_t dash = text.find('-');
  if (dash == std::string_view::npos || dash < 4) {
    return false;
  }
  const std::string_view year_text = text.substr(0, dash);
  if (text.size() < dash + 3 || text[dash + 1] != 'W') {
    return false;
  }
  const std::string_view week_text = text.substr(dash + 2);
  if (week_text.size() != 2) {
    return false;
  }
  year = 0;
  for (const char c : year_text) {
    if (c < '0' || c > '9') {
      return false;
    }
    year = year * 10 + (c - '0');
    if (year > 999999999) {
      return false;
    }
  }
  if (!ParseDigits(week_text, week)) {
    return false;
  }
  return year > 0 && week >= 1 && week <= IsoWeeksInYear(year);
}

bool ParseTime(std::string_view text, TimeParts& out)
{
  if (text.size() < 5 || text[2] != ':') {
    return false;
  }
  TimeParts time;
  if (!ParseDigits(text.substr(0, 2), time.hour) || !ParseDigits(text.substr(3, 2), time.minute)) {
    return false;
  }
  std::size_t pos = 5;
  if (pos < text.size()) {
    if (text[pos] != ':') {
      return false;
    }
    if (pos + 3 > text.size()) {
      return false;
    }
    if (!ParseDigits(text.substr(pos + 1, 2), time.second)) {
      return false;
    }
    time.has_seconds = true;
    pos += 3;
    if (pos < text.size()) {
      if (text[pos] != '.' || pos + 1 >= text.size()) {
        return false;
      }
      time.fraction = text.substr(pos + 1);
      // "One, two, or three ASCII digits, representing the fractional part"
      // (HTML §2.3.5.4).
      if (time.fraction.size() > 3) {
        return false;
      }
      for (const char c : time.fraction) {
        if (c < '0' || c > '9') {
          return false;
        }
      }
    }
  }
  if (time.hour > 23 || time.minute > 59 || time.second > 59) {
    return false;
  }
  out = time;
  return true;
}

std::string TwoDigits(int value)
{
  std::string text;
  text.push_back(static_cast<char>('0' + (value / 10)));
  text.push_back(static_cast<char>('0' + (value % 10)));
  return text;
}

std::string NormalizeTime(const TimeParts& time)
{
  std::string result = TwoDigits(time.hour) + ":" + TwoDigits(time.minute);
  // A valid normalized local date and time string omits the seconds when they
  // are zero, and trims trailing zeros from a fraction.
  bool fraction_nonzero = false;
  for (const char c : time.fraction) {
    if (c != '0') {
      fraction_nonzero = true;
      break;
    }
  }
  if (time.has_seconds && (time.second != 0 || fraction_nonzero)) {
    result += ":" + TwoDigits(time.second);
    if (fraction_nonzero) {
      std::string fraction(time.fraction);
      while (!fraction.empty() && fraction.back() == '0') {
        fraction.pop_back();
      }
      result += "." + fraction;
    }
  }
  return result;
}

std::string FormatDate(const DateParts& date)
{
  // The year is preserved verbatim as four (or more) digits, so a valid
  // "0001-01-01" round-trips rather than losing its leading zeros.
  std::string result = std::to_string(date.year);
  while (result.size() < 4) {
    result.insert(result.begin(), '0');
  }
  return result + "-" + TwoDigits(date.month) + "-" + TwoDigits(date.day);
}

// --- range -----------------------------------------------------------------

std::string SanitizeRangeValue(const dom::Element& input, std::string_view value)
{
  const std::string_view min_attr = input.GetAttribute("min").value_or("");
  const std::string_view max_attr = input.GetAttribute("max").value_or("");
  const std::optional<double> parsed_min = ParseFloatingPoint(min_attr);
  const std::optional<double> parsed_max = ParseFloatingPoint(max_attr);

  const double minimum = parsed_min.value_or(0.0);
  double maximum = parsed_max.value_or(100.0);
  if (maximum < minimum) {
    maximum = minimum;
  }

  // "If the value of the element is not a valid floating-point number, then
  // set it to the best representation, as a floating-point number, of the
  // default value.  The default value is the minimum plus half the difference
  // between the minimum and the maximum, unless the maximum is less than the
  // minimum, in which case the default value is the minimum."
  double result = 0;
  if (const std::optional<double> parsed = ParseFloatingPoint(value); parsed.has_value()) {
    result = *parsed;
  } else {
    result = minimum + ((maximum - minimum) / 2.0);
  }
  // Underflow/overflow: the user agent must set the value to the minimum /
  // maximum (HTML §4.10.5.1.14 Range state).
  result = std::clamp(result, minimum, maximum);

  // Range values are step-aligned.  The step base is the minimum when it is
  // present, otherwise the `value` content attribute when it is present and
  // valid, otherwise zero (HTML §4.10.5.3's step base definition).
  const std::string value_attr = RawAttr(input, "value");
  const std::optional<double> parsed_value = ParseFloatingPoint(value_attr);
  const double step_base = parsed_min.has_value() ? *parsed_min : parsed_value.value_or(0.0);
  const std::string step_attr = RawAttr(input, "step");
  if (!base::AsciiEqualsIgnoreCase(step_attr, "any")) {
    double step = 1.0;
    if (const std::optional<double> parsed_step = ParseFloatingPoint(step_attr);
        parsed_step.has_value() && *parsed_step > 0) {
      step = *parsed_step;
    }
    const double ratio = (result - step_base) / step;
    // Ties round towards positive infinity (the standard encourages that for
    // step mismatch); the epsilon absorbs binary floating-point error such as
    // 0.35 / 0.1 evaluating to 3.4999999999999996.
    const double epsilon = 1e-9 * std::max(1.0, std::abs(ratio));
    result = step_base + (std::floor(ratio + 0.5 + epsilon) * step);
    result = std::clamp(result, minimum, maximum);
  }
  // Serialize with the precision the attributes imply, so a step of 0.1
  // yields "0.3" rather than "0.30000000000000004".
  const int places = std::max({DecimalPlaces(step_attr),
                               DecimalPlaces(min_attr),
                               DecimalPlaces(max_attr),
                               DecimalPlaces(value_attr)});
  result = RoundToDecimalPlaces(result, places);
  return FormatFloatingPoint(result);
}

// --- color -----------------------------------------------------------------

// Serialize a parsed CSS color the way a colour well control does: alpha is
// forced opaque (the engine has no `alpha` attribute) and the result is the
// lowercase simple colour "#rrggbb".  Parsing failures become "#000000".
std::string SanitizeColorValue(std::string_view value)
{
  // The colour well parses the whole value; surrounding whitespace is not
  // accepted (Chrome rejects " red " while accepting "red").
  if (value != TrimAsciiWhitespace(value)) {
    return "#000000";
  }
  const std::optional<css::Color> color = css::ParseColor(value);
  if (!color.has_value()) {
    return "#000000";
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result = "#";
  for (const std::uint8_t component : {color->r, color->g, color->b}) {
    result.push_back(kHex[(component >> 4U) & 0x0FU]);
    result.push_back(kHex[component & 0x0FU]);
  }
  return result;
}

// --- email multiple --------------------------------------------------------

std::string SanitizeEmailList(std::string_view value)
{
  // "Split on commas the element's value, strip leading and trailing ASCII
  // whitespace from each resulting token, if any, and let the element's values
  // be the list; set the element's value to the result of concatenating the
  // element's values, separating each value by a single comma."
  std::string result;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t comma = value.find(',', start);
    const std::string_view token = value.substr(
        start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    if (!result.empty() || start != 0) {
      result.push_back(',');
    }
    result.append(TrimAsciiWhitespace(token));
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return result;
}

} // namespace

std::string InputTypeKeyword(const dom::Element& input)
{
  const std::string type = base::ToLower(RawAttr(input, "type"));
  static constexpr std::string_view kKeywords[] = {
      "hidden", "text",  "search",         "tel",    "url",   "email", "password", "date",  "month",
      "week",   "time",  "datetime-local", "number", "range", "color", "checkbox", "radio", "file",
      "submit", "image", "reset",          "button"};
  for (const std::string_view keyword : kKeywords) {
    if (type == keyword) {
      return std::string(keyword);
    }
  }
  return "text"; // missing value default and invalid value default (§4.10.5.3)
}

InputValueMode InputValueModeFor(std::string_view type_keyword)
{
  if (type_keyword == "checkbox" || type_keyword == "radio") {
    return InputValueMode::kDefaultOn;
  }
  if (type_keyword == "file") {
    return InputValueMode::kFilename;
  }
  if (type_keyword == "submit" || type_keyword == "image" || type_keyword == "reset" ||
      type_keyword == "button") {
    return InputValueMode::kDefault;
  }
  return InputValueMode::kValue;
}

std::string SanitizeInputValue(const dom::Element& input, std::string_view value)
{
  const std::string type = InputTypeKeyword(input);
  if (type == "text" || type == "search" || type == "tel" || type == "password" ||
      type == "hidden") {
    return StripNewlines(value);
  }
  if (type == "url") {
    const std::string stripped = StripNewlines(value);
    return std::string(TrimAsciiWhitespace(stripped));
  }
  if (type == "email") {
    if (input.HasAttribute("multiple")) {
      return SanitizeEmailList(value);
    }
    const std::string stripped = StripNewlines(value);
    return std::string(TrimAsciiWhitespace(stripped));
  }
  if (type == "number") {
    return ParseFloatingPoint(value).has_value() ? std::string(value) : std::string();
  }
  if (type == "range") {
    return SanitizeRangeValue(input, value);
  }
  if (type == "date") {
    DateParts date;
    return ParseDate(value, date) ? std::string(value) : std::string();
  }
  if (type == "month") {
    long long year = 0;
    int month = 0;
    return ParseMonth(value, year, month) ? std::string(value) : std::string();
  }
  if (type == "week") {
    long long year = 0;
    int week = 0;
    return ParseWeek(value, year, week) ? std::string(value) : std::string();
  }
  if (type == "time") {
    TimeParts time;
    return ParseTime(value, time) ? std::string(value) : std::string();
  }
  if (type == "datetime-local") {
    const std::size_t t = value.find('T');
    if (t == std::string_view::npos) {
      return std::string();
    }
    DateParts date;
    TimeParts time;
    if (!ParseDate(value.substr(0, t), date) || !ParseTime(value.substr(t + 1), time)) {
      return std::string();
    }
    return FormatDate(date) + "T" + NormalizeTime(time);
  }
  if (type == "color") {
    return SanitizeColorValue(value);
  }
  if (type == "file") {
    return std::string();
  }
  // default/default-on states do not define a value sanitization algorithm.
  return std::string(value);
}

std::string InputValue(const dom::Element& input)
{
  if (input.tag_name() != "input") {
    return dom::TextControlValue(input);
  }
  switch (InputValueModeFor(InputTypeKeyword(input))) {
  case InputValueMode::kFilename:
    return std::string(); // the engine has no File API: the file list is empty
  case InputValueMode::kDefault:
    return RawAttr(input, "value");
  case InputValueMode::kDefaultOn: {
    const std::optional<std::string_view> value = input.GetAttribute("value");
    return value.has_value() ? std::string(*value) : std::string("on");
  }
  case InputValueMode::kValue:
  default:
    return SanitizeInputValue(input, dom::TextControlValue(input));
  }
}

void SetInputValue(dom::Element& input, std::string_view value)
{
  if (input.tag_name() != "input") {
    return;
  }
  const InputValueMode mode = InputValueModeFor(InputTypeKeyword(input));
  if (mode == InputValueMode::kDefault || mode == InputValueMode::kDefaultOn) {
    // The value IDL attribute is in the default/default-on mode: setting it
    // reflects into the value content attribute.  This is also what makes
    // checkbox/radio `.value` observable through getAttribute('value').
    input.SetAttribute("value", value);
    return;
  }
  dom::TextControlState& state = input.EnsureTextControlState();
  if (mode == InputValueMode::kFilename) {
    state.current_value = std::string();
    state.dirty_value = true;
    return;
  }
  state.current_value = SanitizeInputValue(input, value);
  state.dirty_value = true;
}

void InputValueAttributeChanged(dom::Element& input)
{
  if (input.tag_name() != "input") {
    return;
  }
  const InputValueMode mode = InputValueModeFor(InputTypeKeyword(input));
  if (mode != InputValueMode::kValue) {
    // The default/default-on/filename modes read the attribute directly.
    if (dom::TextControlState* state = input.text_control_state()) {
      state->current_value.reset();
    }
    return;
  }
  dom::TextControlState* state = input.text_control_state();
  if (state != nullptr && state->dirty_value) {
    return; // a dirty value ignores changes to the default value
  }
  if (state == nullptr) {
    return; // nothing stored: the getter derives from the attribute
  }
  state->current_value = SanitizeInputValue(input, RawAttr(input, "value"));
}

void SetInputDefaultValue(dom::Element& input, std::string_view value)
{
  input.SetAttribute("value", value);
  InputValueAttributeChanged(input);
}

void InputTypeChanged(dom::Element& input, std::string_view old_type, std::string_view old_value)
{
  if (input.tag_name() != "input") {
    return;
  }
  const InputValueMode old_mode = InputValueModeFor(old_type);
  const std::string new_type = InputTypeKeyword(input);
  const InputValueMode new_mode = InputValueModeFor(new_type);
  const std::string attribute_value = RawAttr(input, "value");
  dom::TextControlState& state = input.EnsureTextControlState();

  if (old_mode == InputValueMode::kValue && !old_value.empty() &&
      (new_mode == InputValueMode::kDefault || new_mode == InputValueMode::kDefaultOn)) {
    // "If the previous state put the value IDL attribute in the value mode,
    // and the element's value is not the empty string, and the new state puts
    // it in the default or default/on mode, then set the value content
    // attribute to the element's value."
    input.SetAttribute("value", old_value);
  } else if (old_mode != InputValueMode::kValue && new_mode == InputValueMode::kValue) {
    // "Otherwise, if the previous state put it in any mode other than the
    // value mode, and the new state puts it in the value mode, then set the
    // value of the element to the value of the value content attribute, if
    // there is one, or the empty string otherwise, and then set the dirty
    // value flag to false."
    state.current_value = attribute_value;
    state.dirty_value = false;
  } else if (old_mode != InputValueMode::kFilename && new_mode == InputValueMode::kFilename) {
    // "Otherwise, if the previous state put it in any mode other than the
    // filename mode, and the new state puts it in the filename mode, then set
    // the value of the element to the empty string."
    state.current_value = std::string();
  }

  if (new_mode == InputValueMode::kValue) {
    const std::string current =
        old_mode == InputValueMode::kValue ? std::string(old_value) : attribute_value;
    // "Invoke the value sanitization algorithm, if one is defined for the type
    // attribute's new state."
    state.current_value = SanitizeInputValue(input, current);
  } else {
    state.current_value.reset();
  }
}

} // namespace neko::html
