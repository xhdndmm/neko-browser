// neko::browser::RunPageScripts — executes a page's <script> elements
// (inline text or external src=) with DOM bindings (Phase 8 M2 + external
// script loading).  Called on the worker thread right after HTML parsing,
// before the page is published to the UI.
//
// Loading model (see page_scripts.h): classic scripts fetch+run in document
// order (blocking), defer scripts run after all classics in document order,
// and async scripts run after the classic+defer phases in document order
// (a documented approximation of the spec's non-blocking async model).

#include "neko/browser/page_scripts.h"

#include "neko/base/logging.h"
#include "neko/base/string_util.h"
#include "neko/style/computed_style.h"
#include "neko/url/url.h"

#include <ctime>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neko::browser {
namespace {

bool HasHttpOnlyAttribute(std::string_view assignment)
{
  while (!assignment.empty()) {
    const std::size_t separator = assignment.find(';');
    std::string_view attribute = assignment.substr(0, separator);
    while (!attribute.empty() && (attribute.front() == ' ' || attribute.front() == '\t')) {
      attribute.remove_prefix(1);
    }
    while (!attribute.empty() && (attribute.back() == ' ' || attribute.back() == '\t')) {
      attribute.remove_suffix(1);
    }
    if (base::AsciiEqualsIgnoreCase(attribute, "httponly")) {
      return true;
    }
    if (separator == std::string_view::npos) {
      break;
    }
    assignment.remove_prefix(separator + 1);
  }
  return false;
}

// Serializes an element's computed style as property -> resolved value
// (kebab-case keys, px strings) for window.getComputedStyle().  Covers the
// box model, fonts, text and layout-relevant properties the engine computes;
// values are human-readable (StyleEngine's ToString forms).
std::map<std::string, std::string> SerializeComputedStyle(const style::ComputedStyle& s)
{
  std::map<std::string, std::string> out;
  const auto size = [](const std::optional<style::SizeSpec>& spec) {
    return spec.has_value() ? style::ToString(spec.value()) : std::string("auto");
  };
  const auto size4 = [](const style::SizeSpec& a,
                        const style::SizeSpec& b,
                        const style::SizeSpec& c,
                        const style::SizeSpec& d) {
    return style::ToString(a) + " " + style::ToString(b) + " " + style::ToString(c) + " " +
           style::ToString(d);
  };
  out["display"] = std::string(style::ToString(s.display));
  out["position"] = std::string(style::ToString(s.position));
  out["float"] = s.floating == style::Float::kLeft    ? "left"
                 : s.floating == style::Float::kRight ? "right"
                                                      : "none";
  out["width"] = size(s.width);
  out["height"] = size(s.height);
  out["min-width"] = size(s.min_width);
  out["max-width"] = size(s.max_width);
  out["min-height"] = size(s.min_height);
  out["max-height"] = size(s.max_height);
  out["margin"] = size4(s.margin_top, s.margin_right, s.margin_bottom, s.margin_left);
  out["padding"] = size4(s.padding_top, s.padding_right, s.padding_bottom, s.padding_left);
  out["border-top-width"] = style::ToString(s.border_top);
  out["border-right-width"] = style::ToString(s.border_right);
  out["border-bottom-width"] = style::ToString(s.border_bottom);
  out["border-left-width"] = style::ToString(s.border_left);
  out["font-size"] = style::ToString(
      style::SizeSpec{static_cast<float>(s.font_size), false, false, {}, false, false, false, {}});
  out["font-family"] = s.font_family;
  out["font-weight"] = std::to_string(s.font_weight);
  out["line-height"] = std::to_string(s.line_height);
  out["text-align"] = std::string(style::ToString(s.text_align));
  out["color"] =
      s.color.has_value() ? style::ToString(s.color.value()) : std::string("rgb(0, 0, 0)");
  out["background-color"] = s.background_color.has_value()
                                ? style::ToString(s.background_color.value())
                                : std::string("rgba(0, 0, 0, 0)");
  out["overflow"] = s.overflow == style::Overflow::kHidden   ? "hidden"
                    : s.overflow == style::Overflow::kAuto   ? "auto"
                    : s.overflow == style::Overflow::kScroll ? "scroll"
                                                             : "visible";
  out["white-space"] = s.white_space == style::WhiteSpace::kNowrap    ? "nowrap"
                       : s.white_space == style::WhiteSpace::kPre     ? "pre"
                       : s.white_space == style::WhiteSpace::kPreWrap ? "pre-wrap"
                       : s.white_space == style::WhiteSpace::kPreLine ? "pre-line"
                                                                      : "normal";
  out["flex-direction"] = std::string(style::ToString(s.flex_direction));
  out["flex-wrap"] = s.flex_wrap == style::FlexWrap::kWrap          ? "wrap"
                     : s.flex_wrap == style::FlexWrap::kWrapReverse ? "wrap-reverse"
                                                                    : "nowrap";
  out["flex-grow"] = std::to_string(s.flex_grow);
  out["flex-shrink"] = std::to_string(s.flex_shrink);
  out["order"] = std::to_string(s.order);
  return out;
}

// Collects <script> elements in document order (pre-order traversal), so
// scripts run in the order the author wrote them (head before body).
void CollectScripts(const dom::Node& root, std::vector<dom::Element*>& out)
{
  for (dom::Node* child : root.ChildNodes()) {
    if (child->node_type() != dom::NodeType::kElement) {
      continue;
    }
    auto* element = static_cast<dom::Element*>(child);
    if (element->tag_name() == "script") {
      out.push_back(element);
    }
    CollectScripts(*element, out);
  }
}

// True for <script type="module"> (type matching is ASCII case-insensitive
// with surrounding whitespace ignored, per the HTML script-element rules).
bool IsModuleScript(const dom::Element& script)
{
  const std::optional<std::string_view> type = script.GetAttribute("type");
  if (!type.has_value()) {
    return false;
  }
  return base::AsciiEqualsIgnoreCase(base::Trim(type.value()), "module");
}

} // namespace

std::shared_ptr<javascript::DomBinder> RunPageScripts(renderer::Page& page,
                                                      const std::string& base_url,
                                                      ScriptFetcher fetch,
                                                      javascript::ScriptEngine::ConsoleSink sink,
                                                      const PageScriptServices& services,
                                                      ScriptRequestedNavigation* out_navigation)
{
  dom::Document* document = page.document();
  if (document == nullptr) {
    return nullptr;
  }
  std::vector<dom::Element*> scripts;
  CollectScripts(*document, scripts);
  if (scripts.empty()) {
    return nullptr;
  }

  // Keep a local copy of the sink for script-error reporting; the binder's
  // engine takes its own copy for console.log etc.
  javascript::ScriptEngine::ConsoleSink error_sink = sink;

  // Phase 8 M3: wire the browser services (localStorage + fetch) into the
  // page's runtime when the caller provided them.
  javascript::PageApis apis;
  if (services.local_storage != nullptr) {
    storage::LocalStorage* store = services.local_storage;
    const std::string origin = services.origin;
    apis.storage_get = [store, origin](std::string_view key) {
      return store->GetItem(origin, key);
    };
    apis.storage_set = [store, origin](std::string_view key, std::string_view value) {
      store->SetItem(origin, key, value);
    };
    apis.storage_remove = [store, origin](std::string_view key) {
      return store->RemoveItem(origin, key);
    };
    apis.storage_clear = [store, origin]() { store->Clear(origin); };
    apis.storage_keys = [store, origin]() {
      std::vector<std::string> keys;
      for (const auto& entry : store->All(origin)) {
        keys.push_back(entry.first);
      }
      return keys;
    };
  }
  if (services.cookies != nullptr) {
    storage::CookieStore* cookies = services.cookies;
    const base::Result<url::Url> document_url = url::Url::Parse(base_url);
    if (document_url.has_value()) {
      const url::Url current_url = document_url.value();
      apis.cookie_get = [cookies, current_url]() {
        return cookies->DocumentCookieFor(current_url, static_cast<int64_t>(std::time(nullptr)));
      };
      apis.cookie_set = [cookies, current_url](std::string_view assignment) {
        if (HasHttpOnlyAttribute(assignment)) {
          return;
        }
        (void)cookies->SetCookieFromHeader(
            current_url, assignment, static_cast<int64_t>(std::time(nullptr)));
      };
    }
  }
  // window.indexedDB: drive the per-origin IndexedDB store through the same
  // callback pattern as localStorage.
  if (services.indexed_db != nullptr) {
    storage::IndexedDbStore* idb = services.indexed_db;
    const std::string origin = services.origin;
    apis.idb_current_version = [idb, origin](std::string_view db) {
      return idb->CurrentVersion(origin, db);
    };
    apis.idb_create_db = [idb, origin](std::string_view db) {
      return idb->CreateDatabase(origin, db);
    };
    apis.idb_set_version = [idb, origin](std::string_view db, int64_t version) {
      return idb->SetVersion(origin, db, version);
    };
    apis.idb_delete_db = [idb, origin](std::string_view db) {
      return idb->DeleteDatabase(origin, db);
    };
    apis.idb_store_names = [idb, origin](std::string_view db) {
      const base::Result<std::vector<storage::IndexedDbStore::ObjectStoreMeta>> metas =
          idb->ObjectStores(origin, db);
      if (!metas.has_value()) {
        return base::Result<std::vector<javascript::IdbStoreMeta>>(metas.error());
      }
      std::vector<javascript::IdbStoreMeta> out;
      out.reserve(metas.value().size());
      for (const auto& meta : metas.value()) {
        javascript::IdbStoreMeta item;
        item.name = meta.name;
        item.key_path = meta.key_path;
        item.auto_increment = meta.auto_increment;
        out.push_back(std::move(item));
      }
      return base::Result<std::vector<javascript::IdbStoreMeta>>(std::move(out));
    };
    apis.idb_create_store = [idb, origin](std::string_view db,
                                          std::string_view store,
                                          std::string_view key_path,
                                          bool auto_increment) {
      return idb->CreateObjectStore(origin, db, store, key_path, auto_increment);
    };
    apis.idb_delete_store = [idb, origin](std::string_view db, std::string_view store) {
      return idb->DeleteObjectStore(origin, db, store);
    };
    apis.idb_add = [idb, origin](std::string_view db,
                                 std::string_view store,
                                 std::optional<std::string> key,
                                 std::string value) {
      return idb->Add(origin, db, store, std::move(key), std::move(value));
    };
    apis.idb_put = [idb, origin](std::string_view db,
                                 std::string_view store,
                                 std::optional<std::string> key,
                                 std::string value) {
      return idb->Put(origin, db, store, std::move(key), std::move(value));
    };
    apis.idb_get = [idb, origin](std::string_view db, std::string_view store, std::string key) {
      return idb->Get(origin, db, store, std::move(key));
    };
    apis.idb_delete = [idb, origin](std::string_view db, std::string_view store, std::string key) {
      return idb->Delete(origin, db, store, std::move(key));
    };
    apis.idb_clear = [idb, origin](std::string_view db, std::string_view store) {
      return idb->Clear(origin, db, store);
    };
    apis.idb_count = [idb, origin](std::string_view db, std::string_view store) {
      return idb->Count(origin, db, store);
    };
    apis.idb_get_all = [idb, origin](std::string_view db, std::string_view store) {
      return idb->GetAll(origin, db, store);
    };
  }
  // Resolve relative URLs against the page's base URL.  Used by fetch() and
  // window.location assignments.
  const base::Result<url::Url> base = url::Url::Parse(base_url);
  apis.resolve_url = [base](const std::string& raw) -> std::string {
    const base::Result<url::Url> parsed =
        base.has_value() ? url::Url::Parse(raw, base.value()) : url::Url::Parse(raw);
    return parsed.has_value() ? parsed.value().Serialize() : std::string();
  };
  if (fetch != nullptr) {
    // Fetch through the same network path used for external scripts.
    apis.fetch = [fetch](const std::string& url) -> base::Result<javascript::FetchResponse> {
      const base::Result<url::Url> parsed = url::Url::Parse(url);
      if (!parsed.has_value()) {
        return base::Err(base::Error::InvalidArgument("invalid URL"));
      }
      const base::Result<network::HttpResponse> response = fetch(parsed.value());
      if (!response.has_value()) {
        return base::Err(response.error());
      }
      javascript::FetchResponse out;
      out.status = response.value().status_code;
      out.status_text = response.value().reason;
      out.final_url = response.value().final_url;
      out.headers.reserve(response.value().headers.size());
      for (const network::HttpHeader& header : response.value().headers) {
        out.headers.emplace_back(header.name, header.value);
      }
      out.body = response.value().body;
      return base::Ok(std::move(out));
    };
  }
  // window.location: expose the current document URL and record script
  // navigation requests (last one wins) for the caller to act on after the
  // script run.  Relative targets resolve against the page base URL.
  if (out_navigation != nullptr) {
    apis.location_href = [base_url]() { return base_url; };
    apis.navigate = [out_navigation](const std::string& url) { out_navigation->url = url; };
    apis.reload = [out_navigation]() { out_navigation->is_reload = true; };
  } else {
    apis.location_href = [base_url]() { return base_url; };
  }

  // window.getComputedStyle(element): serialize the element's computed style
  // from the page's style engine (the engine keeps per-element styles after
  // ApplyStyles, which RunPageScripts re-runs after DOM mutations).
  apis.computed_style = [&page](const dom::Element& element) -> std::map<std::string, std::string> {
    const style::ComputedStyle& style = page.styles().StyleFor(element);
    return SerializeComputedStyle(style);
  };

  // Element layout geometry (getBoundingClientRect / offsetWidth etc): the
  // union of the element's laid-out border box and inline fragments.
  apis.element_geometry =
      [&page](const dom::Element& element) -> std::optional<javascript::ElementGeometry> {
    const std::optional<renderer::ElementGeometry> g = page.ElementBoxGeometry(element);
    if (!g.has_value()) {
      return std::nullopt;
    }
    javascript::ElementGeometry out;
    out.x = static_cast<double>(g->x);
    out.y = static_cast<double>(g->y);
    out.width = static_cast<double>(g->width);
    out.height = static_cast<double>(g->height);
    out.client_width = static_cast<double>(g->client_width);
    out.client_height = static_cast<double>(g->client_height);
    out.border_top = static_cast<double>(g->border_top);
    out.border_left = static_cast<double>(g->border_left);
    return out;
  };

  // HTMLMediaElement (<video>) controls: drive the renderer's playback state
  // directly (frames advance on the same tick as script timers/GIFs).
  apis.video_play = [&page](const dom::Element& element) { page.PlayVideo(element); };
  apis.video_pause = [&page](const dom::Element& element) { page.PauseVideo(element); };
  apis.video_seek = [&page](const dom::Element& element, double seconds) {
    page.SeekVideo(element, seconds);
  };
  apis.video_duration = [&page](const dom::Element& element) {
    return page.VideoDuration(element);
  };
  apis.video_current_time = [&page](const dom::Element& element) {
    return page.VideoCurrentTime(element);
  };
  apis.video_paused = [&page](const dom::Element& element) {
    return !page.IsVideoPlaying(element);
  };

  auto binder = std::make_shared<javascript::DomBinder>(*document, apis);
  binder->SetConsoleSink(std::move(sink));

  // Runs one classic script body (inline text or fetched external file);
  // failures are logged and do not stop the remaining scripts.  |script|
  // identifies the executing element for document.currentScript (WHATWG HTML
  // §4.12.1): set while the body runs and cleared right after, so the getter
  // is only non-null during the script's synchronous execution.  Classic
  // bodies evaluate under the document URL: dynamic import() inside them
  // resolves relative specifiers against the page, like a browser.
  auto report_error = [&](const std::string& message) {
    NEKO_LOG_WARNING("page script error: " + message);
    if (error_sink) {
      error_sink("error", message);
    }
  };
  auto run_source = [&](dom::Element* script, std::string_view source, std::string_view filename) {
    binder->SetCurrentScript(script);
    const auto result = binder->Evaluate(source, filename);
    binder->SetCurrentScript(nullptr);
    if (!result.has_value()) {
      report_error("Uncaught " + result.error().message());
    }
  };

  // Returns the executable source of one <script>, or nullopt when the
  // element must be skipped (empty, or its external fetch failed).
  auto script_source = [&](dom::Element* script) -> std::optional<std::string> {
    if (script->HasAttribute("src")) {
      const std::optional<std::string_view> src = script->GetAttribute("src");
      base::Result<url::Url> target =
          base.has_value() ? url::Url::Parse(*src, base.value()) : url::Url::Parse(*src);
      if (!target.has_value()) {
        const std::string message = "script: cannot resolve src \"" + std::string(*src) + "\"";
        NEKO_LOG_WARNING(message);
        if (error_sink) {
          error_sink("error", message);
        }
        return std::nullopt;
      }
      const auto response = fetch(target.value());
      if (!response) {
        const std::string message = "script: fetch failed for " + target.value().Serialize() +
                                    ": " + response.error().message();
        NEKO_LOG_WARNING(message);
        if (error_sink) {
          error_sink("error", message);
        }
        return std::nullopt;
      }
      return response.value().body;
    }
    const std::string source = script->TextContent();
    return source.empty() ? std::nullopt : std::optional<std::string>(source);
  };

  // Module scripts (<script type="module">).  The module is evaluated under
  // its absolute URL so relative import specifiers resolve correctly:
  // inline modules use the document URL, external modules their own src.
  // document.currentScript stays null during module evaluation (spec).
  // Entry-module dedup: two elements with the same src evaluate once (the
  // module map caches it; a second JS_Eval under the same name would be a
  // recompile, not the spec's single evaluation).
  std::set<std::string> loaded_module_urls;
  auto run_module_source = [&](std::string_view source, const std::string& url) {
    const auto result = binder->EvaluateModule(source, url);
    if (!result.has_value()) {
      report_error("Uncaught " + result.error().message());
    }
  };
  auto run_module_script = [&](dom::Element* script) {
    if (script->HasAttribute("src")) {
      const std::optional<std::string_view> src = script->GetAttribute("src");
      const base::Result<url::Url> target =
          base.has_value() ? url::Url::Parse(*src, base.value()) : url::Url::Parse(*src);
      if (!target.has_value()) {
        report_error("module script: cannot resolve src \"" + std::string(*src) + "\"");
        return;
      }
      const std::string module_url = target.value().Serialize();
      if (!loaded_module_urls.insert(module_url).second) {
        return; // same-URL entry already evaluated
      }
      const auto response = fetch(target.value());
      if (!response.has_value()) {
        report_error("module script: fetch failed for " + module_url + ": " +
                     response.error().message());
        return;
      }
      run_module_source(response.value().body, module_url);
      return;
    }
    const std::string source = script->TextContent();
    if (!source.empty()) {
      run_module_source(source, base_url);
    }
  };

  // Pass 1: classic scripts (no async, no defer) in document order.  Module
  // scripts never run here — they are deferred by default (see passes 2/3).
  for (dom::Element* script : scripts) {
    if (script->HasAttribute("async") || script->HasAttribute("defer") || IsModuleScript(*script)) {
      continue;
    }
    const std::optional<std::string> source = script_source(script);
    if (source.has_value()) {
      run_source(script, source.value(), base_url);
    }
  }
  // Pass 2: defer scripts and non-async module scripts in document order
  // (they run after parsing, which has already completed — same observable
  // phase as classic here, but after every classic script regardless of
  // source position).
  for (dom::Element* script : scripts) {
    const bool deferred_classic =
        script->HasAttribute("defer") && !script->HasAttribute("async") && !IsModuleScript(*script);
    const bool module_deferred = IsModuleScript(*script) && !script->HasAttribute("async");
    if (!deferred_classic && !module_deferred) {
      continue;
    }
    if (IsModuleScript(*script)) {
      run_module_script(script);
    } else {
      const std::optional<std::string> source = script_source(script);
      if (source.has_value()) {
        run_source(script, source.value(), base_url);
      }
    }
  }
  // Pass 3: async scripts (classic and module) in document order
  // (approximation of the spec's run-when-ready model).
  for (dom::Element* script : scripts) {
    if (!script->HasAttribute("async")) {
      continue;
    }
    if (IsModuleScript(*script)) {
      run_module_script(script);
    } else {
      const std::optional<std::string> source = script_source(script);
      if (source.has_value()) {
        run_source(script, source.value(), base_url);
      }
    }
  }

  // Fire the document lifecycle events now that every script has run:
  // parsing is complete, so DOMContentLoaded then load fire on the document
  // (window-level listeners were registered on the document, so they run
  // too).  Approximation: real browsers fire load only after all subresources
  // finish, which this synchronous model has no signal for.
  binder->DispatchDocumentEvent("DOMContentLoaded");
  binder->DispatchDocumentEvent("load");

  // Scripts may have mutated the DOM; re-run the cascade so layout reflects
  // the new state.
  page.ReapplyStyles();
  return binder;
}

} // namespace neko::browser
