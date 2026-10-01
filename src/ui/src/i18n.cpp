#include "neko/ui/i18n.h"

#include <QApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QString>
#include <QTranslator>

// Q_INIT_RESOURCE expands to a block-scope `extern` declaration, which binds
// to the *enclosing* namespace; it therefore has to sit in the global
// namespace to reference the rcc-generated initializer.  neko_ui is built as
// a static library and the linker discards the rcc object file unless this
// reference (reached through EnsureTranslationResources) pulls it in.
void NekoUiInitTranslationResources()
{
  Q_INIT_RESOURCE(translations);
}

namespace neko::ui::i18n {

void EnsureTranslationResources()
{
  ::NekoUiInitTranslationResources();
}

namespace {

// The translators currently installed by ApplyLanguage/InstallTranslators.
// Removing the previous pair on re-install keeps the application from
// stacking catalogs (which would make lookups order-dependent).
QTranslator* g_ui_translator = nullptr;
QTranslator* g_qt_translator = nullptr;

} // namespace

const std::vector<Language>& SupportedLanguages()
{
  static const std::vector<Language> languages = {
      Language{"en", "English"},
      Language{"zh_CN", "简体中文"},
      Language{"zh_TW", "繁體中文"},
      Language{"ja", "日本語"},
      Language{"ko", "한국어"},
      Language{"ar", "العربية"},
      Language{"ru", "Русский"},
  };
  return languages;
}

bool IsSupportedLanguage(std::string_view id)
{
  for (const Language& language : SupportedLanguages()) {
    if (id == language.id) {
      return true;
    }
  }
  return false;
}

std::string ResolveLanguageId(std::string_view preference_id)
{
  if (IsSupportedLanguage(preference_id)) {
    if (preference_id == "en") {
      return "en";
    }
    return std::string(preference_id);
  }

  const QString name = QLocale::system().name(); // e.g. "zh_TW", "ja_JP"
  if (name.isEmpty()) {
    return "en";
  }
  // Traditional-Chinese locales without their own catalog map to zh_TW.
  if (name.contains(QLatin1String("Hant")) || name.endsWith(QLatin1String("_HK")) ||
      name.endsWith(QLatin1String("_MO"))) {
    return "zh_TW";
  }
  for (const Language& language : SupportedLanguages()) {
    if (language.id == std::string_view("en")) {
      continue; // English is the fallback, checked last by construction
    }
    const QString id = QString::fromLatin1(language.id);
    if (name == id) {
      return language.id;
    }
    // Same language, different country ("ja_JP" -> "ja"): a zh match already
    // resolved through the exact / traditional checks above, so the first
    // prefix match is the intended variant ("zh_CN" for generic zh locales).
    if (name.startsWith(id + QLatin1Char('_'))) {
      return language.id;
    }
    const QString language_tag = name.section(QLatin1Char('_'), 0, 0);
    if (id.section(QLatin1Char('_'), 0, 0) == language_tag) {
      return language.id;
    }
  }
  return "en";
}

bool IsRightToLeft(std::string_view language_id)
{
  // Only Arabic ships as an RTL catalog; extend together with the language
  // list (Hebrew, Persian, ...) when catalogs are added.
  return language_id == "ar";
}

std::string CatalogPath(std::string_view language_id)
{
  if (language_id.empty() || language_id == "en") {
    return {};
  }
  return ":/i18n/neko_" + std::string(language_id) + ".qm";
}

bool InstallTranslators(QApplication& app, std::string_view language_id)
{
  EnsureTranslationResources();
  if (g_ui_translator != nullptr) {
    app.removeTranslator(g_ui_translator);
    delete g_ui_translator;
    g_ui_translator = nullptr;
  }
  if (g_qt_translator != nullptr) {
    app.removeTranslator(g_qt_translator);
    delete g_qt_translator;
    g_qt_translator = nullptr;
  }
  if (language_id.empty() || language_id == "en") {
    return true;
  }

  const std::string path = CatalogPath(language_id);
  auto* ui = new QTranslator(&app);
  if (!ui->load(QString::fromStdString(path))) {
    // A catalog is missing from the resources (e.g. the translation was not
    // built): report the fallback instead of shipping a half-localized UI.
    delete ui;
    return false;
  }
  app.installTranslator(ui);
  g_ui_translator = ui;

  // The Qt base catalog localizes the standard dialogs (QMessageBox buttons,
  // file dialogs).  Qt ships it as qtbase_<language>.qm in the Qt translation
  // directory; absence is fine (only standard-dialog strings stay English).
  auto* qt = new QTranslator(&app);
  const QString qt_dir = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
  const QString language_name =
      QString::fromLatin1(language_id.data(), static_cast<int>(language_id.size()));
  if (qt->load(QStringLiteral("qtbase_") + language_name, qt_dir)) {
    app.installTranslator(qt);
    g_qt_translator = qt;
  } else {
    delete qt;
  }
  return true;
}

std::string ApplyLanguage(QApplication& app, std::string_view preference_id)
{
  const std::string effective = ResolveLanguageId(preference_id);
  if (!InstallTranslators(app, effective)) {
    // Catalog unavailable: stay on the English source strings.
    app.setLayoutDirection(Qt::LeftToRight);
    (void)InstallTranslators(app, "en");
    return "en";
  }
  app.setLayoutDirection(IsRightToLeft(effective) ? Qt::RightToLeft : Qt::LeftToRight);
  return effective;
}

} // namespace neko::ui::i18n
