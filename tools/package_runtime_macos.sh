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
#      dylibs (FFmpeg, OpenSSL, ...) are copied into the bundle and install
#      names are rewritten,
#   2. adds the offscreen platform plugin to the bundle as well (macdeployqt
#      skips it by design) so headless smoke tests can run,
#   3. bundles the CLI's non-Qt dylibs into <staging-dir>/lib (the CLI does not
#      link Qt) and rewrites every reference to @executable_path/../lib/...,
#   4. ad-hoc signs every Mach-O file in the bundle, seals the .app itself and
#      verifies the result with `codesign --verify --deep`,
#   5. verifies that every non-system dependency resolves to a file inside the
#      package (@rpath / @loader_path / @executable_path included).
#
# macdeployqt's own signing pass is disabled when the option exists: it can
# fail half-way through on bundles with mixed Homebrew dependency trees (and
# it never seals the .app itself), so this script signs the finished bundle.
#
# The plugin set macdeployqt deploys comes from the Qt installation it belongs
# to.  CI installs the qtbase keg only (the GUI uses Qt6 Widgets, tests use
# Qt6 Test); the `qt` umbrella formula would additionally expose the plugins
# of qtwebengine (QtPdf), qtvirtualkeyboard, qtsvg, ... — macdeployqt deploys
# iconengines/platforminputcontexts plugins unconditionally, but the matching
# frameworks live in other kegs and cannot be resolved from there.
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
QT_PREFIX="${QT_PREFIX:-}"
if [ -z "$QT_PREFIX" ]; then
  # Prefer qtbase — the module the GUI links against and the only Qt keg CI
  # installs (see the header).  Fall back to the `qt` umbrella prefix for
  # developer machines that installed the full meta formula.
  for candidate in "$BREW_PREFIX/opt/qtbase" "$BREW_PREFIX/opt/qt"; do
    if [ -x "$candidate/bin/macdeployqt" ]; then
      QT_PREFIX="$candidate"
      break
    fi
  done
fi
MACDEPLOYQT="${MACDEPLOYQT:-}"
if [ -z "$MACDEPLOYQT" ]; then
  if [ -n "$QT_PREFIX" ] && [ -x "$QT_PREFIX/bin/macdeployqt" ]; then
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

# Lexically collapse '.' and '..' segments (no symlinks involved) so an LC_RPATH
# such as '@loader_path/../..' is recognised as leaving the package instead of
# being accepted by a string prefix match.
canonicalize_path() {
  local rest part result
  rest="$1"
  result=""
  while [ -n "$rest" ]; do
    case "$rest" in
      */*) part="${rest%%/*}"; rest="${rest#*/}" ;;
      *)   part="$rest"; rest="" ;;
    esac
    case "$part" in
      ''|.) ;;
      '..')
        if [ -n "$result" ]; then result="${result%/*}"; fi ;;
      *) result="$result/$part" ;;
    esac
  done
  if [ -z "$result" ]; then result="/"; fi
  printf '%s\n' "$result"
}

# Drop every LC_RPATH entry that does not lead into the package: every
# non-system dependency is rewritten to an explicit @executable_path /
# @loader_path reference, so a leftover entry can only make dyld load a
# library from the build machine (Homebrew, Xcode, ...) instead of the copy
# shipped in the package.  $2 is the directory @executable_path stands for
# (bin/ for the CLI, Contents/MacOS inside the .app).
delete_external_rpaths() {
  local f="$1" exec_dir="$2" dir rpath expanded
  dir="$(dirname "$f")"
  while read -r rpath; do
    [ -n "$rpath" ] || continue
    expanded="$rpath"
    case "$expanded" in
      @loader_path/*)     expanded="$dir/${expanded#@loader_path/}" ;;
      @executable_path/*) expanded="$exec_dir/${expanded#@executable_path/}" ;;
    esac
    expanded="$(canonicalize_path "$expanded")"
    case "$expanded" in
      "$STAGE_DIR"/*) continue ;;
    esac
    install_name_tool -delete_rpath "$rpath" "$f" >/dev/null 2>&1 || true
  done < <(macho_rpaths "$f")
}

sign_file() {
  mach_o "$1" || return 0
  codesign --force --sign - "$1"
}

# Ad-hoc sign the complete GUI bundle: every nested Mach-O first, then the
# bundle itself (codesign refuses to seal a bundle whose nested code is not
# signed), then verify the result.  macdeployqt's own signing pass is not used
# (see the header): it fails half-way through for our dependency tree and does
# not seal the .app root, which leaves an unsigned bundle behind.
sign_app_bundle() {
  [ -d "$APP_DIR" ] || return 0
  local f output
  while IFS= read -r -d '' f; do
    sign_file "$f"
  done < <(find "$APP_DIR" -type f -print0)

  codesign --force --sign - "$APP_DIR"
  if ! output="$(codesign --verify --deep --verbose=2 "$APP_DIR" 2>&1)"; then
    echo "$output" >&2
    echo "error: app bundle signature verification failed: $APP_DIR" >&2
    exit 1
  fi
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
    delete_external_rpaths "$file" "$BIN_DIR"
    delete_external_rpaths "$LIB_DIR/$base" "$BIN_DIR"
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
  # Recent macdeployqt versions ad-hoc sign the bundle by default.  Skip that
  # pass when supported: it can fail half-way through and leaves stale or
  # missing signatures behind; the script signs the finished bundle itself
  # (see sign_app_bundle).  The option is probed from the usage output so old
  # macdeployqt builds keep working.
  #
  # Argument order is dictated by macdeployqt itself (src/tools/macdeployqt/
  # macdeployqt/main.cpp): argv[1] must be the app bundle — when it starts
  # with '-' the tool prints its usage text and exits 1 — and options are
  # parsed from argv[2] onward.  An option therefore never goes first.
  local help_output
  help_output="$("$MACDEPLOYQT" 2>&1 || true)"
  if grep -q -- '-no-codesign' <<<"$help_output"; then
    "$MACDEPLOYQT" "$APP_DIR" -no-codesign
  else
    "$MACDEPLOYQT" "$APP_DIR"
  fi

  # macdeployqt deliberately skips the offscreen platform plugin; add it back
  # (pointing at the deployed frameworks) so `QT_QPA_PLATFORM=offscreen` works
  # for smoke tests and headless screenshots.
  plugin_dir="${QT_PLUGIN_DIR:-}"
  if [ -z "$plugin_dir" ] && [ -n "$QT_PREFIX" ] && [ -x "$QT_PREFIX/bin/qmake" ]; then
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

  echo "==> Normalizing bundled dependency references"
  normalize_app_bundle

  echo "==> Signing app bundle (ad-hoc)"
  sign_app_bundle

  # The app bundle is one user-visible artifact; the loose GUI binary is gone.
  echo "==> GUI bundled as $(basename "$APP_DIR")"
}

# macdeployqt rewrites a dependency reference only when it resolved that
# dependency without an LC_RPATH (deployQtFrameworks() in
# qtbase/src/tools/macdeployqt/shared/shared.cpp: a dependency with a
# non-empty rpathUsed keeps its '@rpath/<name>' reference; changeIdentification
# and deployRPaths() are likewise only applied to the bundle's *main* binaries).
# A deployed third-party dylib such as Homebrew's libwebp.7.dylib therefore
# keeps both its '/opt/homebrew/...' LC_RPATH and its '@rpath/libsharpyuv.0.dylib'
# reference, even though the target was copied next to it into
# Contents/Frameworks.  On a machine that has that prefix, dyld would then load
# the build machine's copy instead of the bundled one.
#
# Mirror the CLI treatment over the finished bundle: drop LC_RPATHs that leave
# the package and point every reference whose target is deployed under
# Contents/Frameworks at '@executable_path/../Frameworks/<name>'.  References
# that already resolve inside the bundle (@executable_path, @loader_path such
# as ICU's '@loader_path/libicuuc.78.dylib') are left untouched;
# sign_app_bundle re-signs every file this pass modifies.
normalize_app_bundle() {
  [ -d "$APP_DIR" ] || return 0
  local frameworks_dir="$APP_DIR/Contents/Frameworks"
  local exec_dir="$APP_DIR/Contents/MacOS"
  local f ref dir tail base rel
  while IFS= read -r -d '' f; do
    mach_o "$f" || continue
    dir="$(dirname "$f")"
    delete_external_rpaths "$f" "$exec_dir"
    while read -r ref; do
      [ -n "$ref" ] || continue
      is_system_ref "$ref" && continue
      case "$ref" in
        @executable_path/*) continue ;; # already bundle-relative
        @loader_path/*)
          # Same-directory references (ICU's '@loader_path/libicuuc.78.dylib')
          # stay as long as they resolve inside the package; an escaping one
          # falls through and is relocated below.
          case "$(canonicalize_path "$dir/${ref#@loader_path/}")" in
            "$STAGE_DIR"/*) continue ;;
          esac ;;
      esac
      rel=""
      case "$ref" in
        @rpath/*)
          tail="${ref#@rpath/}"
          if [ -e "$frameworks_dir/$tail" ]; then rel="$tail"; fi ;;
        *)
          base="${ref##*/}"
          if [ -e "$frameworks_dir/$base" ]; then rel="$base"; fi ;;
      esac
      [ -n "$rel" ] || {
        case "$ref" in
          /*|@rpath/*)
            printf 'warning: %s: %s is not deployed under Contents/Frameworks\n' "${f#"$STAGE_DIR"/}" "$ref" >&2 ;;
        esac
        continue
      }
      install_name_tool -change "$ref" "@executable_path/../Frameworks/$rel" "$f"
    done < <(macho_deps "$f")
  done < <(find "$APP_DIR" -type f -print0)
}

# --- verification ------------------------------------------------------------

# LC_RPATH entries that dyld may consult when resolving @rpath references of
# $1: the file's own, plus those of the main executable.  dyld accumulates
# rpaths along the load chain and macdeployqt relies on that — framework
# binaries keep @rpath references while only the app binary receives an
# @executable_path/../Frameworks rpath.
resolution_rpaths() {
  local file="$1" main="$2" rpath
  while read -r rpath; do
    if [ -n "$rpath" ]; then printf '%s\n' "$rpath"; fi
  done < <(macho_rpaths "$file")
  if [ -n "$main" ] && [ "$main" != "$file" ] && [ -f "$main" ]; then
    while read -r rpath; do
      if [ -n "$rpath" ]; then printf '%s\n' "$rpath"; fi
    done < <(macho_rpaths "$main")
  fi
}

# Resolve one dependency reference (as printed by otool -L) of $1 to a file
# inside the package.  $3 is the directory @executable_path stands for (bin/
# for the CLI, Contents/MacOS inside the bundle); $4 is the main executable
# whose LC_RPATHs take part in @rpath resolution.  Prints the reference itself
# for system libraries, the resolved path for package files, and nothing when
# the reference does not resolve inside the package.
resolve_packaged_ref() {
  local file="$1" ref="$2" exec_dir="$3" main="$4"
  local dir candidate tail rpath
  dir="$(dirname "$file")"
  case "$ref" in
    /usr/lib/*|/System/*)
      # Part of macOS itself and always present on the target machine.
      printf '%s\n' "$ref"
      return 0 ;;
    @executable_path/*)
      candidate="$exec_dir/${ref#@executable_path/}" ;;
    @loader_path/*)
      candidate="$dir/${ref#@loader_path/}" ;;
    @rpath/*)
      tail="${ref#@rpath/}"
      # dyld takes the first LC_RPATH that contains the file; keep reading the
      # rest of the list (a 'break' here makes the producer's printf fail with
      # EPIPE and spams the log).
      candidate=""
      while read -r rpath; do
        if [ -z "$rpath" ]; then continue; fi
        case "$rpath" in
          @loader_path/*)     rpath="$dir/${rpath#@loader_path/}" ;;
          @executable_path/*) rpath="$exec_dir/${rpath#@executable_path/}" ;;
        esac
        if [ -z "$candidate" ] && [ -e "$rpath/$tail" ]; then
          candidate="$rpath/$tail"
        fi
      done < <(resolution_rpaths "$file" "$main")
      ;;
    *)
      # Absolute references (Homebrew leftovers land here); accepted only when
      # they point inside the package.
      candidate="$ref" ;;
  esac
  if [ -z "$candidate" ]; then
    printf '  %s: no LC_RPATH of the file or of the main executable contains it\n' "$ref" >&2
    return 1
  fi
  if [ ! -e "$candidate" ]; then
    printf '  %s: no file at %s\n' "$ref" "$candidate" >&2
    return 1
  fi
  candidate="$(cd "$(dirname "$candidate")" && pwd -P)/$(basename "$candidate")"
  case "$candidate" in
    "$STAGE_DIR"/*) printf '%s\n' "$candidate" ;;
    *)
      printf '  %s: resolves to %s, which is outside the package\n' "$ref" "$candidate" >&2
      return 1 ;;
  esac
}

# Fail when any dependency of any Mach-O file in the package would not resolve
# on a machine that only has this package (plus macOS itself).  This subsumes
# the old "no reference may point into a Homebrew prefix" check: @rpath,
# @loader_path and @executable_path references are resolved the way dyld
# resolves them (LC_RPATH included), and the resolved file must live inside
# the package.
verify() {
  local rc=0 f ref exec_dir main
  while IFS= read -r -d '' f; do
    mach_o "$f" || continue
    exec_dir="$BIN_DIR"
    main="$BIN_DIR/neko_browser"
    case "$f" in
      "$APP_DIR"/*)
        exec_dir="$APP_DIR/Contents/MacOS"
        main="$APP_DIR/Contents/MacOS/neko_browser_gui" ;;
    esac
    while read -r ref; do
      [ -n "$ref" ] || continue
      if [ -z "$(resolve_packaged_ref "$f" "$ref" "$exec_dir" "$main")" ]; then
        echo "error: ${f#"$STAGE_DIR"/} depends on $ref, which does not resolve inside the package" >&2
        rc=1
      fi
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
