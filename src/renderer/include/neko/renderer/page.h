#pragma once

#include "neko/base/encoding.h"
#include "neko/base/status.h"
#include "neko/dom/element.h"
#include "neko/graphics/font_registry.h"
#include "neko/image/image.h"
#include "neko/image/svg_decoder.h"
#include "neko/layout/layout_tree.h"
#include "neko/paint/rasterizer.h"
#include "neko/style/style_engine.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace neko::base {
class ThreadPool;
}

namespace neko::renderer {

// One occurrence of a find-in-page query: the match's rectangle in device
// pixels (document coordinates — the y is not scroll-adjusted).
struct FindMatch
{
  float x = 0;
  float y = 0;
  float width = 0;
  float height = 0;
};

// User-facing page zoom bounds (Ctrl+= / Ctrl+-), browser-like 25% .. 500%.
inline constexpr float kMinUserZoom = 0.25F;
inline constexpr float kMaxUserZoom = 5.0F;

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

struct CaretGeometry
{
  float x = 0;
  float y = 0;
  float height = 0;
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

  // Returns the focused text caret geometry in document coordinates.
  // Thread-safe; the layout tree is traversed while the page mutex is held.
  std::optional<CaretGeometry> FocusedCaretGeometry() const;

  // Registers parsed external stylesheets (<link rel=stylesheet> content
  // fetched and parsed by the browser application layer) and re-runs the
  // cascade so layout reflects them.
  void SetExternalStylesheets(std::vector<css::StyleSheet> sheets);

  // Replaces the Nth author <style> sheet's parsed content with freshly parsed
  // |text| and re-runs the cascade (document.styleSheets insertRule/deleteRule).
  void SetAuthorSheetText(std::size_t index, const std::string& text);

  // Registers an @font-face web font (bytes already fetched).  Thread-safe;
  // invalidates the layout/paint caches so the next pass uses the face.
  // Returns false when the font data does not parse.
  bool LoadWebFont(const std::string& family,
                   int weight,
                   bool italic,
                   const std::string& key,
                   std::vector<uint8_t> data);

  // True when |key| was already registered via LoadWebFont (callers can
  // skip refetching across reload passes).
  bool HasWebFont(const std::string& key) const;

  // Claims |key| for a font fetch.  Returns false when it was already claimed
  // (loaded or failed) in this document: the post-script stylesheet pass
  // re-runs the font scan with the same declarations, and without this a URL
  // that fails (jd.com references fonts whose host no longer resolves) was
  // re-fetched and re-warned once per pass.  Thread-safe.
  bool ClaimWebFont(const std::string& key);

  // Reads a UTF-8 file and loads it as HTML (encoding sniffing still applies).
  base::Result<void> LoadFile(std::string_view path);

  // Case-insensitive (ASCII) find-in-page over the laid-out text runs, in
  // document order.  Each occurrence reports its rectangle in device pixels,
  // like the other geometry queries.  Matches are found within a single text
  // run — a phrase split across lines or inline elements is not matched — and
  // Unicode case folding beyond ASCII is NOT IMPLEMENTED.  Runs a layout pass
  // first when none exists yet.
  std::vector<FindMatch> FindMatches(std::string_view query);

  // Builds the layout tree at the given viewport width.
  void Layout(float viewport_width, float viewport_height = 0);

  // ADR 0020: the DOM/layout tree is owned by the controller (worker) thread.
  // The controller holds this lock for the duration of any operation that can
  // run page scripts, so its DOM mutations are serialized with the other
  // locked accessors (pool-thread subresource injection, DevTools reads).
  // Recursive: a script callback may re-enter a Page method on the same
  // thread.
  std::unique_lock<std::recursive_mutex> AcquireDomLock();

  // The layout viewport in CSS pixels: the window's viewport divided by the
  // page zoom, i.e. exactly what window.innerWidth/innerHeight must report.
  // Returns 0x0 before the first layout.
  float viewport_css_width() const;
  float viewport_css_height() const;

  // Sets the user-facing page zoom (browser Ctrl+=/Ctrl+-), independent of the
  // CSS `zoom` property: layout runs at viewport/user_zoom CSS pixels while the
  // display list is scaled by user_zoom * CSS zoom, and hit-testing, caret and
  // DOM geometry divide by the same factor.  Scripts therefore see the same
  // CSS-pixel values a browser would report.  |factor| is clamped to
  // [kMinUserZoom, kMaxUserZoom]; re-lays out with the current viewport and
  // returns the applied factor (which equals the previous one when unchanged).
  float SetUserZoom(float factor);
  float user_zoom() const;

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
  // (same buffer size), used by the UI's scroll blit and by the banded
  // screenshot path: the buffer's content was already shifted (or is a fresh
  // band buffer) and only the requested band is redrawn.  Clears the band to
  // the canvas background first.  |y_offset| is the new scroll offset.
  // |pool| enables parallel band rasterization within the band (the output
  // is byte-identical to the serial path).
  void RasterizeInto(paint::Rasterizer& raster,
                     int band_y0,
                     int band_y1,
                     float y_offset,
                     base::ThreadPool* pool = nullptr) const;

  // Monotonic counter bumped whenever the document/style/layout/image content
  // changes.  The UI compares it against its cached rasterization.
  std::uint64_t layout_version() const;

  // Monotonic identity of the currently loaded document.  Unlike document(),
  // this can be read safely while the page is being updated.
  std::uint64_t DocumentVersion() const;

  // Returns whether a layout tree has been built. Thread-safe.
  bool HasLayout() const;

  // Total content height in px after Layout(); 0 before Layout().
  float ContentHeight() const;

  // Glyph-outline provider for SVG <text>/<tspan>: the image decoder has no
  // font stack, so the renderer supplies one built from its own FontRegistry
  // (the same stack the page uses, including @font-face web fonts).  Callers
  // that decode SVG images use this when handing bytes to image::DecodeImage.
  // Thread-safe: the underlying registry is, and the returned shaper caches
  // selectors behind its own mutex.
  image::SvgTextShaper MakeSvgTextShaper() const;

  // Attaches a decoded image to an <img> element (or any element whose
  // computed style has a background-image, which layout resolves through the
  // same ImageProvider lookup) and invalidates the layout so the replaced
  // box picks up its intrinsic size.  |animation| carries the full frame set
  // of an animated GIF; its first frame is installed as the initial image.
  void SetElementImage(const dom::Element* element,
                       image::Image image,
                       std::shared_ptr<image::GifAnimation> animation = nullptr);
  void SetElementImages(const std::vector<const dom::Element*>& elements,
                        const image::Image& image,
                        std::shared_ptr<image::GifAnimation> animation = nullptr);
  void SetElementImage(const dom::Element& element,
                       image::Image image,
                       std::shared_ptr<image::GifAnimation> animation = nullptr)
  {
    SetElementImage(&element, std::move(image), std::move(animation));
  }

  // Records that the fetch/decode of an <img> element's current source
  // failed; the browser layer turns this into the element's "error" event.
  void NoteImageLoadFailed(const dom::Element* element);

  // Drains the image "load"/"error" events queued by SetElementImage(s) and
  // NoteImageLoadFailed, in completion order (second = true for load).
  // Browsers fire these events from their network layer; the engine's image
  // fetcher runs on pool threads while the DOM binder is script-thread
  // confined, so the events are marshalled through the page and dispatched by
  // the browser/CLI layer on the script thread.  Draining (even without a
  // script runtime) also keeps the queue from growing.
  std::vector<std::pair<const dom::Element*, bool>> TakePendingImageEvents();

  // True when |element| still belongs to the current document.  Late passes
  // validate element pointers with this before dispatching queued events (a
  // navigation or a scripted innerHTML may have replaced the subtree).
  bool ContainsElement(const dom::Element* element) const;

  // Paints an axis-aligned rectangle into an element's Canvas 2D backing
  // store using source-over compositing. The store is created at the HTML
  // default size (300x150) on first use.
  void FillCanvasRect(const dom::Element& element,
                      double x,
                      double y,
                      double width,
                      double height,
                      std::array<std::uint8_t, 4> color);

  // A fully decoded video: frame strip + playback metadata.  The browser
  // layer decodes the clip (budgeted) and hands it over together with the
  // first frame.
  struct VideoStrip
  {
    std::shared_ptr<std::vector<image::Image>> frames;
    double frame_rate = 0; // frames per second
    bool loop = false;     // <video loop>
  };

  struct VideoSource
  {
    const dom::Element* element = nullptr;
    std::string url;
    bool autoplay = false;
    bool loop = false;
  };

  std::vector<VideoSource> VideoSources() const;

  std::vector<css::FontFaceRule> FontFaces() const;

  // Attaches a decoded video to a <video> element: |first_frame| becomes the
  // displayed image (the layout's replaced box uses its intrinsic size); the
  // frame strip drives playback.  |autoplay| starts playback on the next
  // AdvanceAnimations tick.
  void SetElementVideo(const dom::Element* element,
                       image::Image first_frame,
                       VideoStrip strip,
                       bool autoplay);
  void SetElementVideo(const dom::Element& element,
                       image::Image first_frame,
                       VideoStrip strip,
                       bool autoplay)
  {
    SetElementVideo(&element, std::move(first_frame), std::move(strip), autoplay);
  }

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

  // Test seam: replaces the monotonic clock (milliseconds) that drives
  // animated images and video playback, so tests can step frame timing
  // deterministically instead of sleeping on the wall clock.  Must be set
  // before the first SetElementImage/SetElementVideo call whose timing is
  // asserted (existing states keep the time they recorded).
  void SetAnimationClockForTesting(std::function<double()> now_ms);

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

  // Copies the computed style for an element while holding the page lock.
  // Returns false when the pointer no longer belongs to the current document.
  bool TryGetComputedStyle(const dom::Element* element,
                           style::ComputedStyle& style,
                           std::string& tag_name) const;

  // Returns image URLs while holding the page lock. Element pointers are
  // non-owning and must be revalidated before a later asynchronous update.
  std::vector<std::pair<const dom::Element*, std::string>> ImageSources() const;

  // Like ImageSources(), but marks each returned (element, URL) pair as
  // claimed so later passes skip it.  Subresource fetchers use this to make
  // repeat passes cheap: the first pass after a load claims every static
  // source, and the passes scheduled from the script/timer pump pick up
  // exactly the sources a page assigned later (lazy loading, src swaps,
  // scripted background-image changes).  Keyed per element *and* URL: an
  // element whose source changes is a new claim, while an unchanged source is
  // never fetched twice; a URL shared by several elements is claimed for each
  // of them (the fetcher still groups by URL, so the network sees one request
  // per pass).
  std::vector<std::pair<const dom::Element*, std::string>> ClaimPendingImageSources();

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
  // Collects <img src> and computed background-image sources; caller must
  // hold mutex_.  Shared by ImageSources() and ClaimPendingImageSources().
  std::vector<std::pair<const dom::Element*, std::string>> CollectImageSourcesLocked() const;
  // Rasterizes every laid-out inline <svg> element into its box through the
  // standalone SVG rasterizer; caller must hold mutex_ and a fresh layout
  // tree.  Sizes/colors are cached so frames do not re-decode per paint.
  void RasterizeInlineSvgImagesLocked();
  // Rebuilds the layout tree; caller must hold mutex_.
  void LayoutLocked(float viewport_width, float viewport_height, bool apply_styles = true);
  void BumpVersion()
  {
    ++version_;
  }

  std::unique_ptr<dom::Document> document_;
  style::StyleEngine styles_;
  std::unique_ptr<layout::LayoutBox> root_;
  float viewport_width_ = 800;
  float viewport_height_ = 0;
  // CSS (root `zoom`) times user zoom; see SetUserZoom().
  float page_zoom_ = 1.0f;
  float user_zoom_ = 1.0f;

  graphics::FontRegistry fonts_;
  std::unordered_map<const dom::Element*, image::Image> images_;
  // (element → claimed source URLs) for ClaimPendingImageSources: what each
  // element has already asked a fetch pass to load.  Cleared with |images_|
  // when the document is replaced.
  std::unordered_map<const dom::Element*, std::unordered_set<std::string>> claimed_image_sources_;
  // Rasterized inline <svg> bitmaps: the used box size and the resolved color
  // decide when the bitmap must be regenerated.  |ok| is false when the
  // element had no drawable content or decoding failed (no retry until the
  // key changes).
  struct InlineSvgRaster
  {
    float width = 0;
    float height = 0;
    std::string color;
    bool ok = false;
  };
  std::unordered_map<const dom::Element*, InlineSvgRaster> inline_svg_rasters_;
  // Image load/error events queued by the fetch layer, drained on the script
  // thread (see TakePendingImageEvents).
  std::vector<std::pair<const dom::Element*, bool>> pending_image_events_;
  // Keys of web fonts already registered (dedup across reload passes).
  std::set<std::string> loaded_webfont_keys_;
  // Keys of web fonts already fetched or attempted (ClaimWebFont).
  std::set<std::string> attempted_webfont_keys_;
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
  std::uint64_t document_version_ = 0;

  // Animation time source (monotonic milliseconds); the steady clock unless a
  // test replaced it.  Guards: mutex_.  Caller of NowMs() must hold mutex_.
  std::function<double()> animation_clock_;
  double NowMs() const;

  // Element with keyboard focus; the UI paints the caret at the end of its
  // text.  Guards: mutex_ (written by the worker, read by the UI).
  const dom::Element* focused_element_ = nullptr;

  // Guards document_/styles_/root_/images_ across the GUI (paint, hit-test)
  // and worker (navigation, image injection) threads.
  mutable std::recursive_mutex mutex_;
};

} // namespace neko::renderer
