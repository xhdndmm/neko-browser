// PSL matching (https://publicsuffix.org/list/): generated rule data in
// public_suffix_data.inc (tools/gen_psl.py), standard longest-match
// algorithm with wildcard (*.ck) and exception (!www.ck) rules.

#include "neko/security/public_suffix.h"

#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace neko::security {
namespace {

#include "public_suffix_data.inc"

struct RuleSet
{
  std::unordered_set<std::string> rules;      // "com", "co.uk"
  std::unordered_set<std::string> wildcards;  // the base of a "*.x" rule -> "ck"
  std::unordered_set<std::string> exceptions; // the labels of a "!x" rule -> "www.ck"
};

const RuleSet& PsList()
{
  static const RuleSet set = [] {
    RuleSet s;
    const auto add = [&s](const char* const* rules, std::size_t count) {
      for (std::size_t i = 0; i < count; ++i) {
        const std::string rule = rules[i];
        if (rule.rfind('!') == 0) {
          s.exceptions.insert(rule.substr(1));
        } else if (rule.rfind("*.") == 0) {
          s.wildcards.insert(rule.substr(2));
        } else {
          s.rules.insert(rule);
        }
      }
    };
    add(kIcannRules, sizeof(kIcannRules) / sizeof(kIcannRules[0]));
    add(kPrivateRules, sizeof(kPrivateRules) / sizeof(kPrivateRules[0]));
    return s;
  }();
  return set;
}

// ASCII-lowercased host with one trailing dot stripped.
std::string Normalize(std::string_view host)
{
  std::string out(host);
  if (!out.empty() && out.back() == '.') {
    out.pop_back();
  }
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

bool IsIpLiteral(const std::string& host)
{
  if (host.find(':') != std::string::npos) {
    return true; // IPv6
  }
  bool any_digit = false;
  for (const char c : host) {
    if (c == '.') {
      continue;
    }
    if (c < '0' || c > '9') {
      return false;
    }
    any_digit = true;
  }
  return any_digit;
}

std::vector<std::string> SplitLabels(const std::string& host)
{
  std::vector<std::string> labels;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= host.size(); ++i) {
    if (i == host.size() || host[i] == '.') {
      labels.push_back(host.substr(start, i - start));
      start = i + 1;
    }
  }
  return labels;
}

// labels[first ..] joined with dots.
std::string Join(const std::vector<std::string>& labels, std::size_t first)
{
  std::string out;
  for (std::size_t i = first; i < labels.size(); ++i) {
    if (!out.empty()) {
      out.push_back('.');
    }
    out += labels[static_cast<std::size_t>(i)];
  }
  return out;
}

} // namespace

std::string PublicSuffix(std::string_view host)
{
  const std::string normalized = Normalize(host);
  if (normalized.empty() || IsIpLiteral(normalized)) {
    return "";
  }
  const std::vector<std::string> labels = SplitLabels(normalized);
  const RuleSet& set = PsList();
  // Longest match wins: scan suffixes from the whole host down to the last
  // label.  An exception rule at any level overrides (its public suffix is
  // the rule minus its leftmost label).
  for (std::size_t first = 0; first < labels.size(); ++first) {
    const std::string suffix = Join(labels, first);
    if (set.exceptions.find(suffix) != set.exceptions.end()) {
      return Join(labels, first + 1);
    }
    const bool normal = set.rules.find(suffix) != set.rules.end();
    const bool wildcard = first + 1 < labels.size() &&
                          set.wildcards.find(Join(labels, first + 1)) != set.wildcards.end();
    if (normal || wildcard) {
      return suffix;
    }
  }
  // The default rule "*": the public suffix is the rightmost label.
  return labels.back();
}

std::string RegistrableDomain(std::string_view host)
{
  const std::string normalized = Normalize(host);
  if (normalized.empty()) {
    return "";
  }
  if (IsIpLiteral(normalized)) {
    return normalized;
  }
  const std::string suffix = PublicSuffix(normalized);
  if (suffix.empty() || suffix == normalized) {
    return "";
  }
  // The registrable domain is the suffix plus exactly one more label.
  const std::string rest = normalized.substr(0, normalized.size() - suffix.size() - 1);
  const std::size_t dot = rest.rfind('.');
  return (dot == std::string::npos ? rest : rest.substr(dot + 1)) + "." + suffix;
}

bool IsPublicSuffix(std::string_view host)
{
  const std::string normalized = Normalize(host);
  if (normalized.empty() || IsIpLiteral(normalized)) {
    return false;
  }
  return PublicSuffix(normalized) == normalized;
}

} // namespace neko::security
