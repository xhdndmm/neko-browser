#include "neko/dom/element.h"
#include "neko/dom/query.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neko::dom {
namespace {

std::unique_ptr<Document> MakeSampleDocument()
{
  auto document = std::make_unique<Document>();

  auto html = std::make_unique<Element>("html");
  auto head = std::make_unique<Element>("head");
  auto title = std::make_unique<Element>("title");
  title->AppendChild(std::make_unique<Text>("Hello Page"));
  head->AppendChild(std::move(title));
  html->AppendChild(std::move(head));

  auto body = std::make_unique<Element>("body");
  auto div = std::make_unique<Element>("div");
  div->SetAttribute("id", "main");
  div->SetAttribute("class", "content note");
  auto p1 = std::make_unique<Element>("p");
  p1->SetAttribute("class", "note");
  p1->AppendChild(std::make_unique<Text>("first"));
  div->AppendChild(std::move(p1));
  auto p2 = std::make_unique<Element>("p");
  p2->AppendChild(std::make_unique<Text>("second"));
  div->AppendChild(std::move(p2));
  body->AppendChild(std::move(div));
  html->AppendChild(std::move(body));

  document->AppendChild(std::move(html));
  return document;
}

TEST(DomTest, TreeStructure)
{
  auto document = MakeSampleDocument();
  ASSERT_EQ(document->child_count(), 1u);
  Node* html = document->first_child();
  ASSERT_NE(html, nullptr);
  EXPECT_EQ(html->node_type(), NodeType::kElement);
  EXPECT_EQ(html->parent(), document.get());
  EXPECT_EQ(html->child_count(), 2u);

  Node* head = html->first_child();
  Node* body = html->last_child();
  ASSERT_NE(head, nullptr);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(static_cast<Element*>(head)->tag_name(), "head");
  EXPECT_EQ(static_cast<Element*>(body)->tag_name(), "body");
  EXPECT_EQ(html->child_at(0), head);
  EXPECT_EQ(html->child_at(1), body);
  EXPECT_EQ(html->child_at(99), nullptr);
}

TEST(DomTest, InsertBeforeAndRemove)
{
  auto document = std::make_unique<Document>();
  auto a = std::make_unique<Element>("a");
  auto b = std::make_unique<Element>("b");
  auto c = std::make_unique<Element>("c");
  Element* b_raw = b.get();
  document->AppendChild(std::move(a));
  document->AppendChild(std::move(b));
  document->InsertBefore(std::move(c), b_raw);

  EXPECT_EQ(document->child_count(), 3u);
  EXPECT_EQ(static_cast<Element*>(document->child_at(1))->tag_name(), "c");
  EXPECT_EQ(document->child_at(2), b_raw);

  std::unique_ptr<Node> removed = document->RemoveChild(b_raw);
  ASSERT_NE(removed, nullptr);
  EXPECT_EQ(static_cast<Element*>(removed.get())->tag_name(), "b");
  EXPECT_EQ(removed->parent(), nullptr);
  EXPECT_EQ(document->child_count(), 2u);
}

TEST(DomTest, RejectsInvalidInsertionsWithoutMutatingTree)
{
  auto parent = std::make_unique<Element>("parent");
  auto foreign_parent = std::make_unique<Element>("foreign");
  foreign_parent->AppendChild(std::make_unique<Element>("foreign-child"));

  EXPECT_FALSE(parent->AppendChild(nullptr));
  EXPECT_EQ(parent->child_count(), 0u);

  EXPECT_FALSE(parent->InsertBefore(std::make_unique<Element>("child"),
                                    foreign_parent->first_child()));
  EXPECT_EQ(parent->child_count(), 0u);

  EXPECT_TRUE(parent->InsertBefore(std::make_unique<Element>("child"), nullptr));
  EXPECT_EQ(parent->child_count(), 1u);
}

TEST(DomTest, RejectsInsertionsBeyondMaximumTreeDepth)
{
  auto root = std::make_unique<Element>("root");
  Node* current = root.get();
  for (std::size_t depth = 1; depth <= Node::kMaximumTreeDepth; ++depth) {
    auto child = std::make_unique<Element>("child");
    Node* raw_child = child.get();
    ASSERT_TRUE(current->AppendChild(std::move(child)));
    current = raw_child;
  }

  EXPECT_FALSE(current->AppendChild(std::make_unique<Element>("too-deep")));
  EXPECT_EQ(current->child_count(), 0u);
}

TEST(DomTest, Attributes)
{
  auto element = std::make_unique<Element>("div");
  EXPECT_FALSE(element->HasAttribute("id"));
  element->SetAttribute("id", "main");
  element->SetAttribute("class", "one two");
  EXPECT_TRUE(element->HasAttribute("id"));
  ASSERT_TRUE(element->GetAttribute("id").has_value());
  EXPECT_EQ(element->GetAttribute("id").value(), "main");
  ASSERT_TRUE(element->Id().has_value());
  EXPECT_EQ(element->Id().value(), "main");

  const std::vector<std::string_view> classes = element->ClassList();
  ASSERT_EQ(classes.size(), 2u);
  EXPECT_EQ(classes[0], "one");
  EXPECT_EQ(classes[1], "two");

  // Overwrite keeps the position.
  element->SetAttribute("id", "other");
  EXPECT_EQ(element->GetAttribute("id").value(), "other");
  element->RemoveAttribute("id");
  EXPECT_FALSE(element->HasAttribute("id"));
}

TEST(DomTest, TextContent)
{
  auto document = MakeSampleDocument();
  Element* html = document->document_element();
  ASSERT_NE(html, nullptr);
  EXPECT_EQ(html->TextContent(), "Hello Pagefirstsecond");
  EXPECT_EQ(document->Title(), "Hello Page");
}

TEST(DomTest, Serialization)
{
  auto document = std::make_unique<Document>();
  auto div = std::make_unique<Element>("div");
  div->SetAttribute("class", "a&b");
  auto text = std::make_unique<Text>("x < y & z");
  div->AppendChild(std::move(text));
  document->AppendChild(std::move(div));
  document->AppendChild(std::make_unique<Comment>("note"));

  EXPECT_EQ(document->ToString(), "<div class=\"a&amp;b\">x &lt; y &amp; z</div><!--note-->");
}

TEST(DomTest, QuerySelectorByTagAndClass)
{
  auto document = MakeSampleDocument();
  Element* main = QuerySelector(*document, "#main");
  ASSERT_NE(main, nullptr);
  EXPECT_EQ(main->tag_name(), "div");

  Element* p = QuerySelector(*document, "p");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->TextContent(), "first");

  const std::vector<Element*> all_p = QuerySelectorAll(*document, "p");
  ASSERT_EQ(all_p.size(), 2u);
  EXPECT_EQ(all_p[0]->TextContent(), "first");
  EXPECT_EQ(all_p[1]->TextContent(), "second");

  Element* body = QuerySelector(*document, "body > div");
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(body->tag_name(), "div");
}

TEST(DomTest, QuerySelectorCompoundAndDescendant)
{
  auto document = MakeSampleDocument();
  // The #main div carries class "content note".
  Element* div_note = QuerySelector(*document, "div.note");
  ASSERT_NE(div_note, nullptr);
  EXPECT_EQ(div_note->tag_name(), "div");
  ASSERT_TRUE(div_note->Id().has_value());
  EXPECT_EQ(div_note->Id().value(), "main");

  Element* nested = QuerySelector(*document, "body p.note");
  ASSERT_NE(nested, nullptr);
  EXPECT_EQ(nested->tag_name(), "p");

  EXPECT_EQ(QuerySelector(*document, "div#missing"), nullptr);
  EXPECT_EQ(QuerySelector(*document, "body > p"), nullptr); // p is not a direct child
}

TEST(DomTest, MatchesCompoundSelector)
{
  auto element = std::make_unique<Element>("span");
  element->SetAttribute("class", "a b");
  EXPECT_TRUE(MatchesCompoundSelector(*element, "span"));
  EXPECT_TRUE(MatchesCompoundSelector(*element, "*.a"));
  EXPECT_TRUE(MatchesCompoundSelector(*element, "span.a.b"));
  EXPECT_FALSE(MatchesCompoundSelector(*element, "div"));
  EXPECT_FALSE(MatchesCompoundSelector(*element, ".c"));
  EXPECT_FALSE(MatchesCompoundSelector(*element, "span#x"));
}

TEST(DomTest, QuerySelectorOnElementRoot)
{
  auto document = MakeSampleDocument();
  Element* div = QuerySelector(*document, "#main");
  ASSERT_NE(div, nullptr);
  EXPECT_EQ(QuerySelector(*div, "p"), div->first_child());
}

// Per DOM spec, the textContent of a Comment node is its data (dom.spec §4.7).
TEST(DomTest, CommentTextContentReturnsData)
{
  auto document = std::make_unique<Document>();
  auto comment = std::make_unique<Comment>(" note ");
  Comment* raw = comment.get();
  document->AppendChild(std::move(comment));
  EXPECT_EQ(raw->TextContent(), " note ");
}

// nodeValue returns the data of Text/Comment nodes and null for other nodes.
TEST(DomTest, NodeValueSemantics)
{
  auto document = std::make_unique<Document>();
  auto text = std::make_unique<Text>("hi");
  Text* raw_text = text.get();
  document->AppendChild(std::move(text));
  auto comment = std::make_unique<Comment>("c");
  Comment* raw_comment = comment.get();
  document->AppendChild(std::move(comment));
  auto div = std::make_unique<Element>("div");
  Element* raw_div = div.get();
  document->AppendChild(std::move(div));

  ASSERT_NE(raw_text->NodeValue(), nullptr);
  EXPECT_EQ(*raw_text->NodeValue(), "hi");
  ASSERT_NE(raw_comment->NodeValue(), nullptr);
  EXPECT_EQ(*raw_comment->NodeValue(), "c");
  EXPECT_EQ(raw_div->NodeValue(), nullptr);
  EXPECT_EQ(document->NodeValue(), nullptr);
}

// Inserting a DocumentFragment moves its children into the parent instead of
// the fragment itself (dom.spec "insert" algorithm).
TEST(DomTest, InsertingDocumentFragmentMovesChildren)
{
  auto parent = std::make_unique<Element>("div");
  auto fragment = std::make_unique<DocumentFragment>();
  fragment->AppendChild(std::make_unique<Text>("a"));
  fragment->AppendChild(std::make_unique<Element>("span"));
  fragment->AppendChild(std::make_unique<Text>("b"));
  parent->AppendChild(std::move(fragment));

  EXPECT_EQ(parent->child_count(), 3u);
  EXPECT_EQ(parent->TextContent(), "ab");
  EXPECT_EQ(parent->child_at(0)->node_type(), NodeType::kText);
  EXPECT_EQ(parent->child_at(1)->node_type(), NodeType::kElement);
  EXPECT_EQ(static_cast<Element*>(parent->child_at(1))->tag_name(), "span");
  EXPECT_EQ(parent->child_at(2)->node_type(), NodeType::kText);

  // InsertBefore a fragment also moves children, before the reference node.
  auto parent2 = std::make_unique<Element>("div");
  parent2->AppendChild(std::make_unique<Text>("tail"));
  Node* tail = parent2->first_child();
  auto fragment2 = std::make_unique<DocumentFragment>();
  fragment2->AppendChild(std::make_unique<Text>("head"));
  parent2->InsertBefore(std::move(fragment2), tail);
  EXPECT_EQ(parent2->child_count(), 2u);
  EXPECT_EQ(parent2->TextContent(), "headtail");
}

// Setting data on a Text node replaces the data (CharacterData.data setter).
TEST(DomTest, TextSetData)
{
  auto text = std::make_unique<Text>("before");
  text->SetData("after");
  EXPECT_EQ(text->data(), "after");
  EXPECT_EQ(*text->NodeValue(), "after");
}

} // namespace
} // namespace neko::dom
