#pragma once

#include "neko/base/encoding.h"
#include "neko/base/status.h"
#include "neko/dom/element.h"
#include "neko/graphics/font_registry.h"
#include "neko/image/image.h"
#include "neko/layout/layout_tree.h"
#include "neko/paint/rasterizer.h"
#include "neko/style/style_engine.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace neko::base {
class ThreadPool;
}

namespace neko::renderer {

// Laid-out geometry of an element in document coordinates (css px), computed
// from the layout tree.  |x|/|y| is the border box origin, |width|/|height|
// the border box size, |client_width|/|client_height| the padding box size and
// |border_top|/|border_left| the top/left border widths.
struct ElementGeometry
{
  float x = 0;
  float y = 0;
  float width = 0;
  float height = 0;
  float client_width = 0;
  float client_height = 0;
  float border_top = 0;
  float border_left = 0;
};

// The minimal page pipeline: HTML -> DOM -> style -> layout -> paint.
//
// Lifecycle: LoadHtml() -> Layout(viewport) -> Rasterize(w, h).
// Headless by design: network fetching happens in the browser application
// (which injects decoded <img> data via SetElementImage).
//
// Performance: the display list is cached and only rebuilt when the document,
// style or layout actually change (tracked by a monotonically increasing
// version).  Rasterize can additionally run on a thread pool (parallel
// horizontal bands); the font caches are thread-safe for that purpose.
class Page : public layout::ImageProvider
{
public:
  Page();

  // Parses |html| into a DOM document and computes styles.  |html| is treated
  // as raw bytes: the character encoding is detected per WHATWG (BOM, then
  // <meta charset> / http-equiv prescan, defaulting to windows-1252) and the
  // bytes are transcoded to UTF-8 before parsing.
  base::Result<void> LoadHtml(std::string_view html);

  // Like LoadHtml but takes the HTTP Content-Type hint (the sniffing
  // algorithm gives the transport-layer encoding priority over the prescan).
  base::Result<void> LoadHtml(std::string_view bytes, base::encoding::Charset http_hint);

  // Re-runs the style cascade over the document.  Needed after page scripts
  // mutate the DOM (attribute/style changes, node insertion) so the layout
  // reflects the new state.
  void ReapplyStyles();

  // Sets the element the pointer currently hovers over (or null), updating the
  // :hover pseudo-class and re-running the cascade/layout so the change is
  // reflected on the next rasterization.  |element| must be in this page's
  // document (or null).
  void SetHoveredElement(const dom::Element* element);

  // Sets the element being activated (mouse button held down), or null,
  // driving the :active pseudo-class.  See SetHoveredElement.
  void SetActiveElement(const dom::Element* element);

  // Sets the element that has keyboard focus (the UI draws the caret at the
  // end of its text), or null when nothing is focused.  Unlike :hover/:active
  // this is written by the browser controller on the worker thread and read
  // by the UI thread while painting the caret.  |element| must be in this
  // page's document (or null).
  void SetFocusedElement(const dom::Element* element);

  // Returns the currently focused element (or null).  Thread-safe.
  const dom::Element* FocusedElement() const;

  // Registers parsed external stylesheets (<link rel=stylesheet> content
  // fetched and parsed by the browser application layer) and re-runs the
  // cascade so layout reflects them.
  void SetExternalStylesheets(std::vector<css::StyleSheet> sheets);

  // Registers an @font-face web font (bytes already fetched).  Thread-safe;
  // invalidates the layout/paint caches so the next pass uses the face.
  // Returns false when the font data does not parse.
  bool LoadWebFont(const std::string& family,
                   int weight,
                   bool italic,
                   const std::string& key,
                   std::vector<uint8_t> data);

  // Reads a UTF-8 file and loads it as HTML (encoding sniffing still applies).
  base::Result<void> LoadFile(std::string_view path);

  // Builds the layout tree at the given viewport width.
  void Layout(float viewport_width, float viewport_height = 0);

  // Rasterizes the laid-out page into a |width| x |height| image.  |y_offset|
  // scrolls the visible region (see paint::Rasterizer::SetScrollOffset).  When
  // |pool| is non-null and the viewport is large enough the viewport is
  // rasterized in parallel horizontal bands.
  paint::Rasterizer
  Rasterize(int width, int height, float y_offset = 0, base::ThreadPool* pool = nullptr) const;

  // Rasterizes the full viewport into an existing rasterizer (its buffer is
  // reused; the font registry and scroll offset are set here).  |pool|
  // enables parallel band rasterization.  Used by the UI's cached repaint.
  void
  RasterizeFull(paint::Rasterizer& raster, float y_offset, base::ThreadPool* pool = nullptr) const;

  // Rasterizes only screen rows [band_y0, band_y1) of an existing rasterizer
  // (same buffer size), used by the UI's scroll blit: the buffer's content was
  // already shifted and only the newly exposed band is redrawn.  Clears the
  // band to the canvas background first.  |y_offset| is the new scroll offset.
  void RasterizeInto(paint::Rasterizer& raster, int band_y0, int band_y1, float y_offset) const;

  // Monotonic counter bumped whenever the document/style/layout/image content
  // changes.  The UI compares it against its cached rasterization.
  std::uint64_t layout_version() const;

  // Total content height in px after Layout(); 0 before Layout().
  float ContentHeight() const;

  // Attaches a decoded image to an <img> element (or any element whose
  // computed style has a background-image, which layout resolves through the
  // same ImageProvider lookup) and invalidates the layout so the replaced
  // box picks up its intrinsic size.  |animation| carries the full frame set
  // of an animated GIF; its first frame is installed as the initial image.
  void SetElementImage(const dom::Element& element,
                       image::Image image,
                       std::shared_ptr<image::GifAnimation> animation = nullptr);

  // A fully decoded video: frame strip + playback metadata.  The browser
  // layer decodes the clip (budgeted) and hands it over together with the
  // first frame.
  struct VideoStrip
  {
    std::shared_ptr<std::vector<image::Image>> frames;
    double frame_rate = 0; // frames per second
    bool loop = false;     // <video loop>
  };

  // Attaches a decoded video to a <video> element: |first_frame| becomes the
  // displayed image (the layout's replaced box uses its intrinsic size); the
  // frame strip drives playback.  |autoplay| starts playback on the next
  // AdvanceAnimations tick.
  void SetElementVideo(const dom::Element& element,
                       image::Image first_frame,
                       VideoStrip strip,
                       bool autoplay);

  // Playback controls (driven by the JS binding through the browser layer).
  void PlayVideo(const dom::Element& element);
  void PauseVideo(const dom::Element& element);
  void SeekVideo(const dom::Element& element, double seconds);
  bool IsVideoPlaying(const dom::Element& element) const;
  std::optional<double> VideoDuration(const dom::Element& element) const;
  std::optional<double> VideoCurrentTime(const dom::Element& element) const;

  // Advances every animated image registered via SetElementImage to the
  // frame for the current time (steady clock; frame durations come from the
  // GIF graphic control extension), and every playing video registered via
  // SetElementVideo to its current frame.  Returns true when at least one
  // frame changed.  A changed frame bumps the layout version so the UI
  // repaints.  Must be driven by a periodic tick (the GUI's 50 ms timer);
  // headless screenshots show the first frame.
  bool AdvanceAnimations();

  // layout::ImageProvider.
  const image::Image* Find(const dom::Element& element) const override;

  // Returns the innermost element whose laid-out content (inline text run or
  // border box) contains the point |x|,|y| in document coordinates (before
  // scroll).  Returns nullptr before Layout() or when the point is outside the
  // laid-out content.  Used for link hit-testing.
  const dom::Element* ElementAt(float x, float y) const;

  // Returns the laid-out geometry of |element| (the union of its block/atomic
  // border box and inline text fragments, in document coordinates), running a
  // layout pass first when none exists yet.  Returns nullopt when the element
  // has no laid-out box (display:none, disconnected, or no document).
  std::optional<ElementGeometry> ElementBoxGeometry(const dom::Element& element);

  dom::Document* document()
  {
    return document_.get();
  }
  const layout::LayoutBox* layout_root() const
  {
    return root_.get();
  }
  const style::StyleEngine& styles() const
  {
    return styles_;
  }

  std::string DumpDom() const;
  std::string DumpLayoutTree() const;

private:
  // Canvas background per CSS propagation: <html> background, else a <body>
  // background, else white.  Paints the whole viewport.
  css::Color CanvasBackgroundColor() const;

  // Rebuilds (if stale) and returns the cached display list.  Caller must hold
  // mutex_.
  const paint::DisplayList& EnsureDisplayList() const;

  void LoadHtmlImpl(std::string_view bytes, base::encoding::Charset charset);
  // Re-runs the cascade and invalidates layout/paint; caller must hold mutex_.
  void ReapplyStylesLocked();
  // Rebuilds the layout tree; caller must hold mutex_.
  void LayoutLocked(float viewport_width, float viewport_height);
  void BumpVersion()
  {
    ++version_;
  }

  std::unique_ptr<dom::Document> document_;
  style::StyleEngine styles_;
  std::unique_ptr<layout::LayoutBox> root_;
  float viewport_width_ = 800;
  float viewport_height_ = 0;
  float page_zoom_ = 1.0f;

  graphics::FontRegistry fonts_;
  std::unordered_map<const dom::Element*, image::Image> images_;

  // Playback state for one animated image (per element).  The frame pixels
  // are kept in |images_| and overwritten in place on each advance so the
  // raw pointers the display list holds stay valid.
  struct ImageAnimationState
  {
    std::shared_ptr<image::GifAnimation> animation;
    double start_ms = 0;   // steady clock of the first display
    std::size_t frame = 0; // currently displayed frame index
    std::size_t loops = 0; // completed full passes
    bool finished = false; // loop count reached; stays on the last frame
  };
  std::unordered_map<const dom::Element*, ImageAnimationState> animation_states_;

  // Playback state for one video (per element).  Frame pixels live in
  // |images_| and are overwritten in place on each advance (same scheme as
  // animated GIFs).
  struct VideoAnimationState
  {
    std::shared_ptr<std::vector<image::Image>> frames;
    double frame_rate = 0;
    bool loop = false;
    bool playing = false; // autoplay starts it on the first advance
    bool autoplay = false;
    double start_ms = 0;    // steady clock when playback (re)started
    double paused_time = 0; // playback position when paused
    std::size_t frame = 0;  // currently displayed frame index
  };
  std::unordered_map<const dom::Element*, VideoAnimationState> video_states_;

  // Cached paint output: rebuilt only when version_ changes.
  mutable std::optional<paint::DisplayList> display_list_;
  mutable std::uint64_t display_list_version_ = 0;

  // Bumped on every content mutation (load, style, layout, image).
  std::uint64_t version_ = 0;

  // Element with keyboard focus; the UI paints the caret at the end of its
  // text.  Guards: mutex_ (written by the worker, read by the UI).
  const dom::Element* focused_element_ = nullptr;

  // Guards document_/styles_/root_/images_ across the GUI (paint, hit-test)
  // and worker (navigation, image injection) threads.
  mutable std::mutex mutex_;
};

} // namespace neko::renderer
