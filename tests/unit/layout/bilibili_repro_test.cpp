// Regressions for real-page layout structures reduced from bilibili's
// homepage.  Each case captures a concrete engine defect that made the site
// render incorrectly:
//
//  * CJK min-content: every CJK run measured as one unbreakable word, so a
//    flex item (min-width:auto) could never shrink to its grid column and a
//    video-card title overflowed its card.
//  * display:-webkit-box: the legacy 2009 flex container fell back to the
//    display initial value (inline), so the title box had no height and the
//    text was clipped away.
//  * calc() with var() and multiplication: `height:calc(2 * var(...))` did
//    not resolve, zeroing title heights.
//  * replaced sizing: percentage widths/heights resolved against the wrong
//    axis (a `width:100%;height:100%` cover image became square, painting
//    over the title area below it), and flex replaced items used intrinsic
//    size instead of the aspect ratio of their resolved width.
//  * percentage height chains through absolutely positioned boxes and flex
//    items: the carousel activity banner stretched its slide image over the
//    whole feed.

#include "neko/dom/query.h"
#include "neko/html/parser.h"
#include "neko/layout/layout_tree.h"
#include "neko/style/style_engine.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>

namespace neko::layout {
namespace {

struct Page
{
  std::unique_ptr<dom::Document> doc;
  style::StyleEngine styles;
  std::unique_ptr<LayoutBox> root;
};

Page Build(std::string_view html, float viewport = 800.0f)
{
  Page page;
  page.doc = html::Parser(html).Parse();
  page.styles.ApplyStyles(*page.doc);
  layout::LayoutEngine engine(page.styles);
  page.root = engine.BuildLayoutTree(*page.doc, viewport);
  return page;
}

const LayoutBox* FindBox(const LayoutBox& box, std::string_view selector, dom::Document& doc)
{
  dom::Element* element = dom::QuerySelector(doc, selector);
  if (element == nullptr) {
    return nullptr;
  }
  if (box.element == element) {
    return &box;
  }
  for (const auto& child : box.children) {
    if (const LayoutBox* found = FindBox(*child, selector, doc)) {
      return found;
    }
  }
  for (const auto& child : box.positioned_children) {
    if (const LayoutBox* found = FindBox(*child, selector, doc)) {
      return found;
    }
  }
  for (const Line& line : box.lines) {
    for (const InlineBox& ib : line.boxes) {
      if (ib.block_box != nullptr) {
        if (const LayoutBox* found = FindBox(*ib.block_box, selector, doc)) {
          return found;
        }
      }
    }
  }
  return nullptr;
}

std::size_t CountRuns(const LayoutBox& box)
{
  std::size_t count = 0;
  for (const Line& line : box.lines) {
    count += line.runs.size();
  }
  for (const auto& child : box.children) {
    count += CountRuns(*child);
  }
  return count;
}

// The recommendation card: a 56.25% padding-top cover area with an absolutely
// positioned cover picture, and a flex info row holding the title h3
// (display:-webkit-box, height:calc(2 * line-height)) and the owner row.
constexpr std::string_view kCardCss = R"css(
  .bili-video-card{position:relative;--title-font-size: 15px;--title-line-height: 22px;
    --subtitle-font-size: 13px;--subtitle-line-height: 17px;--info-margin-top: 10px;
    --icon-size: 18px;--title-padding-right: 24px;--cover-radio: 56.25%}
  .bili-video-card__image--link{display:block}
  .bili-video-card__image{position:relative;z-index:1;border-radius:6px}
  .bili-video-card__image--wrap{padding-top:var(--cover-radio);border-radius:inherit;
    background-color:#f6f7f8;cursor:pointer}
  .v-img{position:relative;display:inline-flex;width:100%;height:100%;background-color:#f6f7f8}
  .v-img img{display:block;width:100%;height:100%;object-fit:inherit}
  .bili-video-card .bili-video-card__cover{position:absolute;top:0;left:0;z-index:1;
    overflow:hidden;width:100%;height:100%;border-radius:6px;object-fit:cover}
  .bili-video-card .bili-video-card__mask{position:absolute;top:0;left:0;z-index:2;
    width:100%;height:100%}
  .bili-video-card .bili-video-card__stats{position:absolute;bottom:0;left:0;z-index:2;
    box-sizing:border-box;padding:16px 8px 6px;width:100%;height:38px;color:#fff;
    display:flex;align-items:center}
  .bili-video-card .bili-video-card__info{display:flex;margin-top:var(--info-margin-top);
    min-height:calc(var(--title-line-height) * 2 + var(--subtitle-line-height) + 4px)}
  .bili-video-card .bili-video-card__info--right{flex:1;position:relative}
  .bili-video-card .bili-video-card__info--tit{padding-right:var(--title-padding-right);
    font-size:var(--title-font-size);line-height:var(--title-line-height);
    height:calc(2 * var(--title-line-height));display:-webkit-box;overflow:hidden;
    -webkit-box-orient:vertical;text-overflow:ellipsis;word-break:break-word}
  .bili-video-card .bili-video-card__info--tit>a{font-weight:500}
  .bili-video-card .bili-video-card__info--bottom{margin-top:4px;
    font-size:var(--subtitle-font-size);display:flex;align-items:center}
  .bili-video-card .bili-video-card__info--owner{cursor:pointer;display:inline-flex;
    align-items:center}
  .bili-video-card .bili-video-card__info--author{flex:1;display:-webkit-box;overflow:hidden}
  .bili-video-card .bili-video-card__info--date{margin-left:4px;
    line-height:var(--subtitle-line-height)}
)css";

constexpr std::string_view kCardHtml = R"html(
<div style="width:270px">
  <div class="bili-video-card is-rcmd" style="--cover-radio: 56.25%">
    <a class="bili-video-card__image--link" href="https://www.bilibili.com/video/BV1">
      <div class="bili-video-card__image">
        <div class="bili-video-card__image--wrap">
          <picture class="v-img bili-video-card__cover">
            <img src="cover.png" alt="">
          </picture>
          <div class="bili-video-card__mask">
            <div class="bili-video-card__stats"><span>138.7万</span><span>02:12</span></div>
          </div>
        </div>
      </div>
    </a>
    <div class="bili-video-card__info">
      <div class="bili-video-card__info--right">
        <h3 class="bili-video-card__info--tit" title="标题">
          <a href="https://www.bilibili.com/video/BV1">美国总统特朗普！卖的黄金手机！到底什么样？竟然中国制造？</a>
        </h3>
        <div class="bili-video-card__info--bottom">
          <a class="bili-video-card__info--owner">
            <span class="bili-video-card__info--author">HOLA小测佬</span>
            <span class="bili-video-card__info--date">· 21小时前</span>
          </a>
        </div>
      </div>
    </div>
  </div>
</div>
)html";

TEST(BilibiliCardRepro, CardFitsItsColumn)
{
  Page page =
      Build(std::string("<style>") + std::string(kCardCss) + "</style>" + std::string(kCardHtml));
  const LayoutBox* card = FindBox(*page.root, ".bili-video-card", *page.doc);
  const LayoutBox* right = FindBox(*page.root, ".bili-video-card__info--right", *page.doc);
  const LayoutBox* title = FindBox(*page.root, ".bili-video-card__info--tit", *page.doc);
  const LayoutBox* info = FindBox(*page.root, ".bili-video-card__info", *page.doc);
  const LayoutBox* wrap = FindBox(*page.root, ".bili-video-card__image--wrap", *page.doc);
  ASSERT_NE(card, nullptr);
  ASSERT_NE(right, nullptr);
  ASSERT_NE(title, nullptr);
  ASSERT_NE(info, nullptr);
  ASSERT_NE(wrap, nullptr);
  // The cover area is a 56.25% padding box of the card width.
  EXPECT_FLOAT_EQ(wrap->height, 270.0f * 0.5625f);
  // The flex item holding the title must be clamped to the card (CJK
  // min-content), not its max-content width.
  EXPECT_FLOAT_EQ(right->width, 270.0f);
  EXPECT_FLOAT_EQ(title->width, 270.0f);
  // display:-webkit-box => block-level flex container with the resolved
  // height (calc over custom properties).
  EXPECT_EQ(title->style.display, style::Display::kFlex);
  EXPECT_FLOAT_EQ(title->height, 44.0f);
  // The info block's min-height: calc(2*22 + 17 + 4) = 65.
  EXPECT_GE(info->height, 65.0f);
  // The title text wraps inside the h3 instead of overflowing it.
  EXPECT_GT(CountRuns(*title), 2u);
  for (const Line& line : title->lines) {
    for (const TextRun& run : line.runs) {
      EXPECT_LE(run.x + run.width, title->x + title->width + 0.01f)
          << "title run must stay inside the h3";
    }
  }
}

TEST(BilibiliCardRepro, CjkFlexItemShrinksToItsColumn)
{
  Page page =
      Build("<style>.flex{display:flex;width:270px}"
            ".item{flex:1}</style>"
            "<div class=flex><div "
            "class=item>美国总统特朗普！卖的黄金手机！到底什么样？竟然中国制造？</div></div>");
  const LayoutBox* item = FindBox(*page.root, ".item", *page.doc);
  ASSERT_NE(item, nullptr);
  EXPECT_FLOAT_EQ(item->width, 270.0f) << "a CJK flex item must shrink to the container";
  EXPECT_GT(item->lines.size(), 1u) << "the text must wrap inside the narrowed item";
}

TEST(BilibiliCardRepro, WebkitBoxIsABlockLevelFlexContainer)
{
  Page page = Build("<style>.t{display:-webkit-box;height:44px;overflow:hidden;"
                    "-webkit-box-orient:vertical}</style>"
                    "<body><div style=width:270px><h3 class=t>美国总统特朗普！</h3></div></body>");
  const LayoutBox* h3 = FindBox(*page.root, ".t", *page.doc);
  ASSERT_NE(h3, nullptr);
  EXPECT_EQ(h3->style.display, style::Display::kFlex);
  EXPECT_EQ(h3->style.flex_direction, style::FlexDirection::kColumn);
  EXPECT_FLOAT_EQ(h3->width, 270.0f);
  EXPECT_FLOAT_EQ(h3->height, 44.0f);
}

TEST(BilibiliCardRepro, MinMaxContentRespectCjkBreakOpportunities)
{
  // 中文文字三字经常见词组: each ideograph is a break opportunity, so the
  // min-content width is one character, not the whole run.
  Page page = Build("<style>.f{display:flex;width:120px}"
                    ".i{flex:1}</style>"
                    "<div class=f><div class=i>中文文字三字经常见词组</div></div>");
  const LayoutBox* item = FindBox(*page.root, ".i", *page.doc);
  ASSERT_NE(item, nullptr);
  EXPECT_FLOAT_EQ(item->width, 120.0f);
}

TEST(BilibiliCardRepro, HeaderBannerGeometry)
{
  // bilibili's header banner: a fixed-height (min 155, 9.375vw, max 240)
  // relative box holding an absolutely-positioned inline-flex picture that is
  // 100% x 100%, with the art <img> inside (a ~10.7:1 panorama; the test uses
  // 3840x360 like the real mirror-report banner).
  Page page = Build(
      "<style>"
      ".bili-header{position:relative;width:100%;min-width:1000px;min-height:64px}"
      ".bili-header__banner{position:relative;z-index:0;display:flex;justify-content:center;"
      "margin:0 auto;min-width:1000px;min-height:155px;height:9.375vw;max-height:240px;"
      "background-color:#e3e5e7}"
      ".bili-header__banner .banner-img{position:absolute;object-fit:cover}"
      ".v-img{position:relative;display:inline-flex;width:100%;height:100%;background-color:#"
      "f6f7f8}"
      ".header-banner__inner{display:flex;align-items:flex-end}"
      "img{display:block;object-fit:cover}"
      "</style>"
      "<div class=bili-header><div class=bili-header__banner>"
      "<picture class='v-img banner-img'><img src=art.png alt='' width=3840 height=360></picture>"
      "<div class=header-banner__inner><a>logo</a></div>"
      "</div><div style='height:600px'>feed</div></div>");
  const LayoutBox* banner = FindBox(*page.root, ".bili-header__banner", *page.doc);
  const LayoutBox* picture = FindBox(*page.root, ".banner-img", *page.doc);
  const LayoutBox* image = FindBox(*page.root, ".banner-img img", *page.doc);
  ASSERT_NE(banner, nullptr);
  ASSERT_NE(picture, nullptr);
  ASSERT_NE(image, nullptr);
  EXPECT_LE(banner->height, 240.0f);
  EXPECT_GE(banner->height, 155.0f);
  // The picture is absolutely positioned at 100% x 100% of the banner.
  EXPECT_FLOAT_EQ(picture->height, banner->height);
  // The art image stretches to the picture (100% height chain + flex stretch).
  EXPECT_FLOAT_EQ(image->height, banner->height);
  EXPECT_FLOAT_EQ(image->width, banner->width);
}

TEST(BilibiliCardRepro, CarouselGeometry)
{
  // bilibili's recommendation carousel (the mirror-report activity banner): a
  // 2x2 grid area whose shim (skeleton cards) establishes the height, with an
  // absolutely-positioned 100%-height chain down to the slide image.  Every
  // level is width/height:100%, so the body/slide must be exactly as tall as
  // the swipe area -- a broken percentage-height link let the image fall back
  // to its intrinsic 550px height (stretched over the whole feed).
  Page page = Build(
      "<style>"
      ".container{display:grid;grid-gap:20px;grid-template-columns:repeat(4,1fr);width:1020px}"
      ".recommended-swipe{grid-column:1/3;grid-row:1/3;--cover-radio:56.25%}"
      ".recommended-swipe-core{position:relative;width:100%}"
      ".recommended-swipe-shim{width:100%;display:grid;grid-gap:20px 12px;"
      "grid-template-columns:repeat(2,1fr)}"
      ".shim-card{width:100%;height:0;padding-top:var(--cover-radio)}"
      ".recommended-swipe-body{position:absolute;top:0;left:0;right:0;bottom:0;display:flex;"
      "flex-direction:column;overflow:hidden;background-color:#f6f7f8}"
      ".recommended-swipe-body-normal{position:absolute;top:0;left:0;right:0;bottom:0;display:flex}"
      ".carousel{position:relative;width:100%;height:100%;z-index:0}"
      ".carousel-container{position:relative;width:100%;height:100%}"
      ".vui_carousel{position:relative;width:100%;height:100%;overflow:hidden}"
      ".vui_carousel .vui_carousel__slides{display:flex;width:100%;height:100%}"
      ".vui_carousel .vui_carousel__slide{flex-shrink:0;width:100%;height:100%;overflow:hidden}"
      ".carousel-area{position:relative}"
      ".carousel-item{display:block;width:100%;height:100%;position:relative}"
      ".v-img{position:relative;display:inline-flex;width:100%;height:100%;background-color:#eee}"
      "img{display:block;object-fit:cover}"
      "</style>"
      "<div class=container><div class=recommended-swipe><div class=recommended-swipe-core>"
      "<div class=recommended-swipe-shim>"
      "<div class=shim-card></div><div class=shim-card></div>"
      "<div class=shim-card></div><div class=shim-card></div>"
      "</div>"
      "<div class=recommended-swipe-body><div class=recommended-swipe-body-normal>"
      "<div class=carousel><div class=carousel-container><div class=vui_carousel>"
      "<div class=vui_carousel__slides><div class=vui_carousel__slide>"
      "<div class=carousel-area><a class=carousel-item>"
      "<picture class='v-img carousel-inner__img'><img src=slide.png alt='' width=976 "
      "height=550></picture>"
      "</a></div></div></div></div></div></div></div></div>"
      "</div></div></div>");
  const LayoutBox* swipe = FindBox(*page.root, ".recommended-swipe", *page.doc);
  const LayoutBox* body = FindBox(*page.root, ".recommended-swipe-body", *page.doc);
  const LayoutBox* slide = FindBox(*page.root, ".vui_carousel__slide", *page.doc);
  const LayoutBox* picture = FindBox(*page.root, ".carousel-inner__img", *page.doc);
  ASSERT_NE(swipe, nullptr);
  ASSERT_NE(body, nullptr);
  ASSERT_NE(slide, nullptr);
  ASSERT_NE(picture, nullptr);
  // The 100% chain down to the slide: the body and slide must be exactly the
  // swipe height.  The slide image keeps its intrinsic aspect ratio at the
  // resolved width (and is clipped by the slide's overflow when taller).
  EXPECT_FLOAT_EQ(body->height, swipe->height) << "body must fill the swipe (100% height chain)";
  EXPECT_FLOAT_EQ(slide->height, swipe->height);
  EXPECT_LE(picture->height, slide->height + 0.01f)
      << "slide image must not exceed the clipped slide area";
  EXPECT_FLOAT_EQ(picture->height, picture->width * 550.0f / 976.0f)
      << "slide image height must follow its intrinsic ratio";
}

} // namespace
} // namespace neko::layout
