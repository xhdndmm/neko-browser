#!/usr/bin/env bash
# Regenerates the Qt translation sources (.ts) from the GUI sources and
# rebuilds the compiled catalogs (.qm).
#
# The .qm files are committed because some build environments (e.g. the CI
# Qt packages installed with aqtinstall qtbase) do not ship the Qt Linguist
# tools; src/ui/CMakeLists.txt falls back to the committed .qm when lrelease
# is unavailable.  Run this script (with Qt Linguist available) whenever UI
# strings change, then commit both the .ts and the .qm files.
#
# usage: tools/update_translations.sh
set -euo pipefail
cd "$(dirname "$0")/.."

LUPDATE="${LUPDATE:-lupdate6}"
LRELEASE="${LRELEASE:-lrelease6}"
I18N_DIR="src/ui/i18n"
LANGS=(zh_CN zh_TW ja ko ar ru)

mkdir -p "$I18N_DIR"

for lang in "${LANGS[@]}"; do
  "$LUPDATE" src/ui -ts "$I18N_DIR/neko_${lang}.ts" -no-obsolete > /dev/null
  "$LRELEASE" "$I18N_DIR/neko_${lang}.ts" -qm "$I18N_DIR/neko_${lang}.qm" > /dev/null
  echo "updated $I18N_DIR/neko_${lang}.ts/.qm"
done

echo "done: remember to commit the .ts and .qm changes"
