#pragma once

#include <string>
#include <string_view>
#include <vector>

class QApplication;

namespace neko::ui::i18n {

// One selectable UI language.  |id| is the profile preference value and the
// .qm catalog suffix; |native_name| is the endonym shown in the settings
// picker.  English is the source language: it has no catalog, but is listed
// so users can force it regardless of the system locale.
struct Language
{
  const char* id;
  const char* native_name;
};

// English plus every language with a shipped catalog (src/ui/i18n/neko_*.ts).
const std::vector<Language>& SupportedLanguages();

// True when |id| is a selectable language id (including "en").
bool IsSupportedLanguage(std::string_view id);

// The language a session should use: |preference_id| when it names a
// supported language, otherwise the best match for the system locale, or
// "en" when nothing matches.  A leading language tag matches its languages
// ("ja_JP" -> "ja"); traditional-Chinese locales without an exact catalog
// entry (zh_HK, zh_MO, *Hant*) resolve to zh_TW, everything else Chinese to
// zh_CN.
std::string ResolveLanguageId(std::string_view preference_id);

// True for right-to-left scripts (Arabic): the whole application layout is
// mirrored.
bool IsRightToLeft(std::string_view language_id);

// Resource path of the compiled catalog for |language_id|, or an empty
// string for English / unknown ids.
std::string CatalogPath(std::string_view language_id);

// Registers the compiled catalogs embedded in the neko_ui library.  Required
// once per process before loading catalogs directly; InstallTranslators and
// ApplyLanguage call it themselves.  (The catalogs live in a Qt resource
// inside the static neko_ui library, and a static library's resource object
// is dropped by the linker unless one of its symbols is referenced.)
void EnsureTranslationResources();

// Installs the UI translator for |language_id|, plus the matching Qt base
// catalog so standard dialogs (QMessageBox buttons, ...) are localized too.
// The translators are owned by |app|.  Returns false when a non-English
// language was requested but its catalog could not be loaded (callers then
// fall back to English).  Calling it again uninstalls the previous pair, so
// the application never stacks translators.
bool InstallTranslators(QApplication& app, std::string_view language_id);

// Startup helper: resolves |preference_id| (empty = follow the system
// locale), installs the translators and sets the layout direction.  Returns
// the effective language id, which may differ from the request when the
// catalog was unavailable.
std::string ApplyLanguage(QApplication& app, std::string_view preference_id);

} // namespace neko::ui::i18n
