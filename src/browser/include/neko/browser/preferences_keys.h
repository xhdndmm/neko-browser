#pragma once

#include <string_view>

namespace neko::browser::prefs {

// Preference keys stored in the profile's preferences.txt (all values are
// strings; booleans use "1"/"0").
//
//   search_engine           built-in engine id (see search_engines.h)
//   search_engine_template  optional custom "%s" URL template; overrides the
//                           built-in engine when set
//   home_page               URL the Home button opens (empty = about:blank)
//   show_bookmark_bar       "1"/"0", bookmark bar visible
inline constexpr std::string_view kSearchEngine = "search_engine";
inline constexpr std::string_view kSearchEngineTemplate = "search_engine_template";
inline constexpr std::string_view kHomePage = "home_page";
inline constexpr std::string_view kShowBookmarkBar = "show_bookmark_bar";

} // namespace neko::browser::prefs
