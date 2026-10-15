#include "fuzz_targets.h"

#include "neko/css/parser.h"
#include "neko/css/tokenizer.h"
#include "neko/dom/element.h"
#include "neko/html/parser.h"
#include "neko/html/token.h"
#include "neko/html/tokenizer.h"
#include "neko/network/http.h"
#include "neko/url/url.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neko::fuzz {
namespace {

// Walks |root|'s subtree iteratively (an explicit stack, so the harness can
// never overflow the call stack on deeply nested input and mask — or invent —
// a parser finding).  Exercises every node accessor a parser bug could leave
// in an inconsistent state.
void WalkDom(const dom::Node& root)
{
  std::vector<const dom::Node*> stack{&root};
  while (!stack.empty()) {
    const dom::Node* node = stack.back();
    stack.pop_back();
    switch (node->node_type()) {
    case dom::NodeType::kElement:
      (void)dom::SerializeOpenTag(*static_cast<const dom::Element*>(node));
      break;
    case dom::NodeType::kText:
    case dom::NodeType::kComment:
      (void)node->TextContent();
      break;
    default:
      break;
    }
    for (const dom::Node* child : node->ChildNodes()) {
      stack.push_back(child);
    }
  }
}

} // namespace

void Url(const std::uint8_t* data, std::size_t size)
{
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  const auto parsed = url::Url::Parse(input);
  if (parsed.has_value()) {
    (void)parsed.value().Serialize(/*include_fragment=*/false);
    (void)parsed.value().Serialize(/*include_fragment=*/true);
    (void)parsed.value().Origin();
  }

  // Relative resolution: the first half is the base, the second the reference.
  const std::size_t half = input.size() / 2;
  const auto base = url::Url::Parse(input.substr(0, half));
  if (base.has_value()) {
    const auto resolved = url::Url::Parse(input.substr(half), base.value());
    if (resolved.has_value()) {
      (void)resolved.value().Serialize();
    }
  }
}

void Http(const std::uint8_t* data, std::size_t size)
{
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const auto response = network::ParseHttpResponse(input);
  if (response.has_value()) {
    // Touch the parsed surface so structurally wrong results (huge body
    // lengths, header count overflow) still get exercised downstream.
    (void)response.value().status_code;
    (void)response.value().GetHeader("content-length");
    (void)response.value().headers.size();
    (void)response.value().body.size();
    for (const network::HttpHeader& header : response.value().headers) {
      (void)header.name.size();
      (void)header.value.size();
    }
  }
}

void Html(const std::uint8_t* data, std::size_t size)
{
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  // Tokenizer: must always terminate at kEOF.
  html::Tokenizer tokenizer(input);
  while (true) {
    const html::Token token = tokenizer.Next();
    if (token.type == html::TokenType::kEOF) {
      break;
    }
    (void)token.name.size();
  }

  // Tree construction: must produce a finite, acyclic DOM for any input.
  html::Parser parser(input);
  std::unique_ptr<dom::Document> document = parser.Parse();
  WalkDom(*document);
}

void Css(const std::uint8_t* data, std::size_t size)
{
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  css::Tokenizer tokenizer(input);
  (void)tokenizer.Tokenize();

  const css::StyleSheet sheet = css::ParseStyleSheet(input);
  (void)sheet.rules.size();

  (void)css::ParseDeclarationBlock(input);
}

} // namespace neko::fuzz
