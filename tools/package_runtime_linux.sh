#!/usr/bin/env bash
# Bundles the runtime dependencies of a packaged neko-browser build for Linux
# into <staging-dir>/lib so the release archive runs on a machine that has
# neither Qt6 nor FFmpeg/OpenSSL/... installed (see ADR 0019).
#
# Usage:
#   tools/package_runtime_linux.sh <staging-dir>
#
# The staging directory must already contain the executables under bin/
# (neko_browser and, when the GUI is built, neko_browser_gui).  The script:
#
#   1. copies the Qt platform/image plugins into <staging-dir>/plugins
#      (source directory from `qmake6 -query QT_INSTALL_PLUGINS`, override
#      with QT_PLUGIN_DIR) and writes bin/qt.conf pointing at them,
#   2. walks the transitive shared-library closure of the binaries and the
#      plugins and copies every non-system library into <staging-dir>/lib,
#   3. rewrites RPATHs so the binaries, libraries and plugins resolve each
#      other relative to their own location ($ORIGIN),
#   4. verifies that every remaining dependency resolves either inside the
#      package or to a host system library.
#
# Deliberately NOT bundled (must come from the host):
#   - glibc and friends: getaddrinfo/NSS (DNS), the dynamic loader and the
#     libc ABI are part of the host system,
#   - the GPU/driver stack (libGL/libEGL/libdrm/...): must match the
#     installed kernel driver.
set -euo pipefail

STAGE_DIR="${1:?usage: tools/package_runtime_linux.sh <staging-dir>}"
STAGE_DIR="$(cd "$STAGE_DIR" && pwd)"
BIN_DIR="$STAGE_DIR/bin"
LIB_DIR="$STAGE_DIR/lib"
PLUGIN_DIR="$STAGE_DIR/plugins"

if [ ! -d "$BIN_DIR" ]; then
  echo "error: $BIN_DIR does not exist (copy the executables first)" >&2
  exit 1
fi
if ! command -v patchelf >/dev/null 2>&1; then
  echo "error: patchelf not found (apt-get install patchelf)" >&2
  exit 1
fi
mkdir -p "$LIB_DIR" "$PLUGIN_DIR"

# --- helpers ----------------------------------------------------------------

is_elf() {
  [ -f "$1" ] || return 1
  [ "$(od -An -N4 -tx1 -- "$1" | tr -d ' \n')" = "7f454c46" ]
}

is_host_lib() {
  case "$(basename "$1")" in
    # glibc / loader / NSS companions
    ld-linux*|linux-vdso.so*|libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|\
    librt.so.*|libresolv.so.*|libnsl.so.*|libutil.so.*|libanl.so.*|libthread_db.so.*)
      return 0 ;;
    # GPU / kernel-driver stack
    libGL.so.*|libGLX.so.*|libEGL.so.*|libOpenGL.so.*|libGLESv2.so.*|\
    libGLdispatch.so.*|libdrm.so.*|libgbm.so.*|libvdpau.so.*|libcuda.so.*|\
    libnvcuvid.so.*|libva.so.*|libva-drm.so.*|libvulkan.so.*)
      return 0 ;;
  esac
  return 1
}

# Resolved direct dependencies of one ELF object.  Fails when one of them
# cannot be resolved at all.
deps_of() {
  local out
  out="$(ldd "$1" 2>/dev/null || true)"
  if grep -q 'not found' <<<"$out"; then
    echo "error: $(basename "$1") has unresolved dependencies:" >&2
    grep 'not found' <<<"$out" >&2
    return 1
  fi
  awk '/=>/ { if ($3 != "" && $3 != "not") print $3 }' <<<"$out"
}

declare -A BUNDLED=()

# Transitively copy everything the given ELF object needs.  Copies land under
# the dependency's SONAME (ldd reports the name the loader looks up), so the
# runtime resolves them without the original symlink chain.
collect_deps() {
  local queue=() dep base sub
  while read -r dep; do
    [ -n "$dep" ] && queue+=("$dep")
  done < <(deps_of "$1")

  while [ "${#queue[@]}" -gt 0 ]; do
    dep="${queue[0]}"
    queue=("${queue[@]:1}")
    is_host_lib "$dep" && continue
    base="$(basename "$dep")"
    if [ -n "${BUNDLED[$base]:-}" ]; then
      continue
    fi
    BUNDLED[$base]=1
    cp -fL -- "$dep" "$LIB_DIR/$base"
    while read -r sub; do
      [ -n "$sub" ] && queue+=("$sub")
    done < <(deps_of "$LIB_DIR/$base")
  done
}

# --- 1. Qt plugins and qt.conf ---------------------------------------------

copy_plugins() {
  local src="${QT_PLUGIN_DIR:-}" candidate rel copied=0
  if [ -z "$src" ]; then
    for candidate in qmake6 qmake; do
      if command -v "$candidate" >/dev/null 2>&1; then
        src="$("$candidate" -query QT_INSTALL_PLUGINS)"
        break
      fi
    done
  fi
  if [ -z "$src" ] || [ ! -d "$src" ]; then
    echo "error: Qt plugin directory not found (set QT_PLUGIN_DIR)" >&2
    return 1
  fi
  for rel in \
    platforms/libqxcb.so \
    platforms/libqwayland.so \
    platforms/libqwayland-generic.so \
    platforms/libqwayland-egl.so \
    platforms/libqoffscreen.so \
    platforms/libqminimal.so \
    imageformats/libqgif.so \
    imageformats/libqico.so \
    imageformats/libqjpeg.so; do
    if [ -f "$src/$rel" ]; then
      mkdir -p "$PLUGIN_DIR/$(dirname "$rel")"
      cp -fL -- "$src/$rel" "$PLUGIN_DIR/$rel"
      copied=$((copied + 1))
    fi
  done
  if [ "$copied" -eq 0 ]; then
    echo "error: no Qt plugins found under $src" >&2
    return 1
  fi
  if [ ! -f "$PLUGIN_DIR/platforms/libqxcb.so" ]; then
    echo "warning: platforms/libqxcb.so not found under $src;" >&2
    echo "         the GUI needs an X11 platform plugin" >&2
  fi
}

# Qt reads qt.conf from the executable's directory; `Prefix = ..` makes both
# the plugin path below and any other Qt path relative to the package root.
write_qt_conf() {
  cat > "$BIN_DIR/qt.conf" <<'EOF'
[Paths]
Prefix = ..
Plugins = plugins
EOF
}

# --- 2. library closure ------------------------------------------------------

bundle_libraries() {
  local entry
  while IFS= read -r -d '' entry; do
    is_elf "$entry" || continue
    collect_deps "$entry"
  done < <(find "$BIN_DIR" "$PLUGIN_DIR" -type f -print0)
}

# --- 3. RPATH rewriting ------------------------------------------------------

patch_rpaths() {
  local dir="$1" rpath="$2" f
  while IFS= read -r -d '' f; do
    is_elf "$f" || continue
    patchelf --set-rpath "$rpath" "$f"
  done < <(find "$dir" -type f -print0)
}

# --- 4. verification ---------------------------------------------------------

verify() {
  local rc=0 f dep deps canon
  while IFS= read -r -d '' f; do
    is_elf "$f" || continue
    if ! deps="$(deps_of "$f")"; then
      rc=1
      continue
    fi
    while read -r dep; do
      [ -n "$dep" ] || continue
      is_host_lib "$dep" && continue
      # ldd reports $ORIGIN-based hits as e.g. <bin>/../lib/libfoo.so, so
      # compare canonical paths.
      canon="$(readlink -f -- "$dep" 2>/dev/null || printf '%s' "$dep")"
      case "$canon" in
        "$LIB_DIR"/*) continue ;;
      esac
      echo "error: $(basename "$f") resolves $(basename "$dep") outside the package:" >&2
      echo "       $dep" >&2
      rc=1
    done <<<"$deps"
  done < <(find "$STAGE_DIR" -type f -print0)
  return "$rc"
}

# --- main --------------------------------------------------------------------

if [ -f "$BIN_DIR/neko_browser_gui" ]; then
  echo "==> Qt plugins"
  copy_plugins
  write_qt_conf
else
  echo "==> no GUI binary; skipping Qt plugin deployment"
fi

echo "==> Bundling shared libraries"
bundle_libraries

echo "==> Rewriting RPATHs"
# Executables look next to themselves, libraries among themselves, plugins two
# levels up (plugins/<category>/<file>.so).  The values must stay literal --
# patchelf records them verbatim for the dynamic loader to expand at runtime.
# shellcheck disable=SC2016
patch_rpaths "$BIN_DIR" '$ORIGIN/../lib'
# shellcheck disable=SC2016
patch_rpaths "$LIB_DIR" '$ORIGIN'
# shellcheck disable=SC2016
patch_rpaths "$PLUGIN_DIR" '$ORIGIN/../../lib'

echo "==> Verifying runtime closure"
verify

echo "==> OK: $(find "$LIB_DIR" -maxdepth 1 -type f | wc -l) libraries bundled into lib/"
