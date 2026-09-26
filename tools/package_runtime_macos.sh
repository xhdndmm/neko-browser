#!/usr/bin/env bash
# Bundles the runtime dependencies of a packaged neko-browser build for macOS
# so the release archive runs without Homebrew Qt / FFmpeg / OpenSSL installed
# (see ADR 0019).
#
# Usage:
#   tools/package_runtime_macos.sh <staging-dir> <version>
#
# The staging directory must already contain the executables under bin/
# (neko_browser and, when the GUI is built, neko_browser_gui).  The script:
#
#   1. turns the GUI binary into a real .app bundle (writing Info.plist) and
#      runs macdeployqt on it: Qt frameworks, Qt plugins *and* the non-Qt
#      dylibs (FFmpeg, OpenSSL, ...) are copied into the bundle, install names
#      are rewritten and everything is ad-hoc signed,
#   2. adds the offscreen platform plugin to the bundle as well (macdeployqt
#      skips it by design) so headless smoke tests can run,
#   3. bundles the CLI's non-Qt dylibs into <staging-dir>/lib (the CLI does not
#      link Qt) and rewrites every reference to @executable_path/../lib/...,
#   4. ad-hoc signs every Mach-O file it touched,
#   5. verifies that no dependency still points into a Homebrew prefix.
#
# System libraries (/usr/lib, /System/...) are never touched: on macOS they
# cannot be static and must match the host.
set -euo pipefail

STAGE_DIR="${1:?usage: tools/package_runtime_macos.sh <staging-dir> <version>}"
VERSION="${2:?usage: tools/package_runtime_macos.sh <staging-dir> <version>}"
STAGE_DIR="$(cd "$STAGE_DIR" && pwd)"
BIN_DIR="$STAGE_DIR/bin"
LIB_DIR="$STAGE_DIR/lib"
APP_DIR="$STAGE_DIR/neko_browser_gui.app"

if [ ! -d "$BIN_DIR" ]; then
  echo "error: $BIN_DIR does not exist (copy the executables first)" >&2
  exit 1
fi
mkdir -p "$LIB_DIR"

BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /usr/local)"
QT_PREFIX="${QT_PREFIX:-$BREW_PREFIX/opt/qt}"
MACDEPLOYQT="${MACDEPLOYQT:-}"
if [ -z "$MACDEPLOYQT" ]; then
  if [ -x "$QT_PREFIX/bin/macdeployqt" ]; then
    MACDEPLOYQT="$QT_PREFIX/bin/macdeployqt"
  else
    MACDEPLOYQT="$(command -v macdeployqt || true)"
  fi
fi

# --- helpers ----------------------------------------------------------------

mach_o() {
  case "$(file -b "$1" 2>/dev/null || true)" in
    *Mach-O*) return 0 ;;
    *) return 1 ;;
  esac
}

# Dependency paths only; never the file's own install name.  otool -L prints
# the install name first only when the file has an LC_ID_DYLIB load command
# (true for dylibs, false for executables and bundles such as Qt plugins).
macho_deps() {
  local skip=0
  if otool -l "$1" 2>/dev/null | grep -q 'LC_ID_DYLIB'; then
    skip=1
  fi
  otool -L "$1" | tail -n +2 | tail -n +$((skip + 1)) | awk '{print $1}'
}

# LC_RPATH entries of one Mach-O file.
macho_rpaths() {
  otool -l "$1" | awk '
    $1 == "cmd" && $2 == "LC_RPATH" { want=1; next }
    want && $1 == "path"            { print $2; want=0 }'
}

# Resolve one dependency reference (as printed by otool) to an existing path,
# relative to the file that references it.  Prints nothing when it cannot be
# resolved.
resolve_ref() {
  local file="$1" ref="$2" dir rpath tail
  dir="$(dirname "$file")"
  case "$ref" in
    /*)
      [ -e "$ref" ] && printf '%s\n' "$ref"
      return 0 ;;
    @loader_path/*)
      tail="${ref#@loader_path/}"
      [ -e "$dir/$tail" ] && printf '%s\n' "$dir/$tail"
      return 0 ;;
    @rpath/*)
      tail="${ref#@rpath/}"
      while read -r rpath; do
        [ -n "$rpath" ] || continue
        rpath="${rpath//@loader_path/$dir}"
        rpath="${rpath//@executable_path/$dir}"
        if [ -e "$rpath/$tail" ]; then
          printf '%s\n' "$rpath/$tail"
          return 0
        fi
      done < <(macho_rpaths "$file")
      if [ -e "$BREW_PREFIX/lib/$tail" ]; then
        printf '%s\n' "$BREW_PREFIX/lib/$tail"
      fi
      return 0 ;;
  esac
  return 0
}

is_system_ref() {
  case "$1" in
    /usr/lib/*|/System/*) return 0 ;;
  esac
  return 1
}

# Drop LC_RPATH entries pointing into a Homebrew prefix: every non-system
# dependency is rewritten to an explicit @executable_path reference, so the
# stale entries would only risk resolving something from the build machine.
delete_brew_rpaths() {
  local f="$1" rpath
  while read -r rpath; do
    [ -n "$rpath" ] || continue
    case "$rpath" in
      "$BREW_PREFIX"/*) install_name_tool -delete_rpath "$rpath" "$f" >/dev/null 2>&1 || true ;;
    esac
  done < <(macho_rpaths "$f")
}

sign_file() {
  mach_o "$1" || return 0
  codesign --force --sign - "$1"
}

# --- CLI: bundle every non-system dylib into lib/ ----------------------------

# Walks the dependency closure of the CLI (which does not link Qt; the GUI is
# handled by macdeployqt) and lands every non-system dylib in $LIB_DIR.
bundle_cli() {
  local entry="$1"
  local queue=() ref resolved base
  while read -r ref; do
    [ -n "$ref" ] && queue+=("$entry|$ref")
  done < <(macho_deps "$entry")

  while [ "${#queue[@]}" -gt 0 ]; do
    local item="${queue[0]}"
    queue=("${queue[@]:1}")
    local file="${item%%|*}"
    local dep="${item#*|}"
    is_system_ref "$dep" && continue
    case "$dep" in
      @executable_path/../lib/*) continue ;; # already rewritten to the bundle
    esac
    resolved="$(resolve_ref "$file" "$dep")"
    if [ -z "$resolved" ]; then
      echo "error: cannot resolve dependency '$dep' of $file" >&2
      exit 1
    fi
    base="$(basename "$resolved")"
    if [ ! -e "$LIB_DIR/$base" ]; then
      cp -fL -- "$resolved" "$LIB_DIR/$base"
      install_name_tool -id "@executable_path/../lib/$base" "$LIB_DIR/$base"
      # The copy's own dependencies still point at Homebrew; queue them.
      while read -r sub; do
        [ -n "$sub" ] && queue+=("$LIB_DIR/$base|$sub")
      done < <(macho_deps "$LIB_DIR/$base")
    fi
    if [ "$dep" != "@executable_path/../lib/$base" ]; then
      install_name_tool -change "$dep" "@executable_path/../lib/$base" "$file"
    fi
    # Rewriting references invalidates signatures; re-sign the touched files.
    delete_brew_rpaths "$file"
    delete_brew_rpaths "$LIB_DIR/$base"
    sign_file "$file"
    sign_file "$LIB_DIR/$base"
  done
}

# --- GUI: assemble a .app bundle and let macdeployqt fill it -----------------

bundle_gui() {
  local gui="$BIN_DIR/neko_browser_gui" plugin plugin_dir
  [ -f "$gui" ] || return 0
  if [ -z "$MACDEPLOYQT" ] || [ ! -x "$MACDEPLOYQT" ]; then
    echo "error: macdeployqt not found (set MACDEPLOYQT or QT_PREFIX)" >&2
    exit 1
  fi

  rm -rf "$APP_DIR"
  mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources"
  cp -fL "$gui" "$APP_DIR/Contents/MacOS/neko_browser_gui"
  rm -f "$gui"

  cat > "$APP_DIR/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>neko_browser_gui</string>
  <key>CFBundleIdentifier</key><string>io.github.neko-browser</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>Neko Browser</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF

  echo "==> macdeployqt"
  "$MACDEPLOYQT" "$APP_DIR"

  # macdeployqt deliberately skips the offscreen platform plugin; add it back
  # (pointing at the deployed frameworks) so `QT_QPA_PLATFORM=offscreen` works
  # for smoke tests and headless screenshots.
  plugin_dir="${QT_PLUGIN_DIR:-}"
  if [ -z "$plugin_dir" ] && [ -x "$QT_PREFIX/bin/qmake" ]; then
    plugin_dir="$("$QT_PREFIX/bin/qmake" -query QT_INSTALL_PLUGINS)"
  fi
  if [ -n "$plugin_dir" ] && [ -f "$plugin_dir/platforms/libqoffscreen.dylib" ]; then
    mkdir -p "$APP_DIR/Contents/PlugIns/platforms"
    plugin="$APP_DIR/Contents/PlugIns/platforms/libqoffscreen.dylib"
    cp -fL "$plugin_dir/platforms/libqoffscreen.dylib" "$plugin"
    while read -r ref; do
      [ -n "$ref" ] || continue
      case "$ref" in
        /usr/lib/*|/System/*) continue ;;
      esac
      local tail
      tail="$(sed -n -E 's#.*(Qt[^/]*\.framework/Versions/[^/]+/[^/]+)$#\1#p' <<<"$ref")"
      if [ -n "$tail" ] && [ -e "$APP_DIR/Contents/Frameworks/$tail" ]; then
        install_name_tool -change "$ref" "@executable_path/../Frameworks/$tail" "$plugin"
      else
        echo "warning: cannot relocate '$ref' of libqoffscreen.dylib" >&2
      fi
    done < <(macho_deps "$plugin")
    sign_file "$plugin"
  else
    echo "warning: libqoffscreen.dylib not found; skipping offscreen plugin" >&2
  fi

  # The app bundle is one user-visible artifact; the loose GUI binary is gone.
  echo "==> GUI bundled as $(basename "$APP_DIR")"
}

# --- verification ------------------------------------------------------------

verify() {
  local rc=0 f ref
  while IFS= read -r -d '' f; do
    mach_o "$f" || continue
    while read -r ref; do
      [ -n "$ref" ] || continue
      case "$ref" in
        /usr/lib/*|/System/*) continue ;;
        @executable_path/../lib/*) continue ;;
        @executable_path/../Frameworks/*) continue ;;
      esac
      echo "error: $(basename "$f") still references $ref" >&2
      rc=1
    done < <(macho_deps "$f")
  done < <(find "$STAGE_DIR" -type f -print0)
  return "$rc"
}

# --- main --------------------------------------------------------------------

if [ -f "$BIN_DIR/neko_browser" ]; then
  echo "==> Bundling CLI dependencies"
  bundle_cli "$BIN_DIR/neko_browser"
fi

bundle_gui

echo "==> Verifying runtime references"
verify

echo "==> OK: $(find "$LIB_DIR" -maxdepth 1 -type f | wc -l | tr -d ' ') CLI libraries bundled into lib/"
