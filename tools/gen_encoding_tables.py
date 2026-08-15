#!/usr/bin/env python3
"""Generate C++ encoding tables for neko-browser's base::encoding module.

Reads the authoritative WHATWG data:
  * encodings.json  (https://encoding.spec.whatwg.org/encodings.json)
  * index-*.txt     (https://raw.githubusercontent.com/whatwg/encoding/main/)

and emits two .inc headers consumed by src/base/src/encoding.cpp:

  * encoding_labels.inc — label -> Charset mapping (sorted, for binary search)
  * encoding_data.inc   — single-byte and multi-byte pointer->code-point tables

Usage:
  python3 tools/gen_encoding_tables.py <data-dir> [<out-dir>]

<data-dir> must contain encodings.json plus the index-*.txt files.  If a file
is missing the script downloads it (network required).  Output defaults to
src/base/src/ (the two .inc files are committed so builds do not need network).
"""

from __future__ import annotations

import json
import os
import sys
import urllib.request

INDEX_URL = "https://raw.githubusercontent.com/whatwg/encoding/main/{}.txt"
ENCODINGS_URL = "https://encoding.spec.whatwg.org/encodings.json"

# WHATWG encoding name -> index file base name.
SINGLE_BYTE_FILES = {
    "IBM866": "index-ibm866",
    "ISO-8859-2": "index-iso-8859-2",
    "ISO-8859-3": "index-iso-8859-3",
    "ISO-8859-4": "index-iso-8859-4",
    "ISO-8859-5": "index-iso-8859-5",
    "ISO-8859-6": "index-iso-8859-6",
    "ISO-8859-7": "index-iso-8859-7",
    "ISO-8859-8": "index-iso-8859-8",
    "ISO-8859-8-I": "index-iso-8859-8",
    "ISO-8859-10": "index-iso-8859-10",
    "ISO-8859-13": "index-iso-8859-13",
    "ISO-8859-14": "index-iso-8859-14",
    "ISO-8859-15": "index-iso-8859-15",
    "ISO-8859-16": "index-iso-8859-16",
    "KOI8-R": "index-koi8-r",
    "KOI8-U": "index-koi8-u",
    "macintosh": "index-macintosh",
    "windows-874": "index-windows-874",
    "windows-1250": "index-windows-1250",
    "windows-1251": "index-windows-1251",
    "windows-1252": "index-windows-1252",
    "windows-1253": "index-windows-1253",
    "windows-1254": "index-windows-1254",
    "windows-1255": "index-windows-1255",
    "windows-1256": "index-windows-1256",
    "windows-1257": "index-windows-1257",
    "windows-1258": "index-windows-1258",
    "x-mac-cyrillic": "index-x-mac-cyrillic",
}

MULTI_BYTE_FILES = {
    "GB18030": "index-gb18030",
    "GB18030-RANGES": "index-gb18030-ranges",
    "Big5": "index-big5",
    "EUC-JP": "index-jis0208",
    "JIS0212": "index-jis0212",
    "EUC-KR": "index-euc-kr",
}

# WHATWG encoding name -> C++ Charset enumerator (must match encoding.h).
ENUM_NAMES = {
    "UTF-8": "kUtf8",
    "IBM866": "kIbm866",
    "ISO-8859-2": "kIso88592",
    "ISO-8859-3": "kIso88593",
    "ISO-8859-4": "kIso88594",
    "ISO-8859-5": "kIso88595",
    "ISO-8859-6": "kIso88596",
    "ISO-8859-7": "kIso88597",
    "ISO-8859-8": "kIso88598",
    "ISO-8859-8-I": "kIso88598I",
    "ISO-8859-10": "kIso885910",
    "ISO-8859-13": "kIso885913",
    "ISO-8859-14": "kIso885914",
    "ISO-8859-15": "kIso885915",
    "ISO-8859-16": "kIso885916",
    "KOI8-R": "kKoi8R",
    "KOI8-U": "kKoi8U",
    "macintosh": "kMacintosh",
    "windows-874": "kWindows874",
    "windows-1250": "kWindows1250",
    "windows-1251": "kWindows1251",
    "windows-1252": "kWindows1252",
    "windows-1253": "kWindows1253",
    "windows-1254": "kWindows1254",
    "windows-1255": "kWindows1255",
    "windows-1256": "kWindows1256",
    "windows-1257": "kWindows1257",
    "windows-1258": "kWindows1258",
    "x-mac-cyrillic": "kXMacCyrillic",
    "GBK": "kGb18030",  # GBK's decoder IS the gb18030 decoder
    "gb18030": "kGb18030",
    "Big5": "kBig5",
    "EUC-JP": "kEucJp",
    "ISO-2022-JP": "kIso2022Jp",
    "Shift_JIS": "kShiftJis",
    "EUC-KR": "kEucKr",
    "replacement": "kReplacement",
    "UTF-16BE": "kUtf16Be",
    "UTF-16LE": "kUtf16Le",
    "x-user-defined": "kXUserDefined",
}

# Pointer-space size for each multi-byte index (direct-indexed arrays).
POINTER_SPACES = {
    "index-gb18030": 23940,   # (0xFE-0x81+1) * 190
    "index-big5": 19782,      # (0xFE-0x81+1) * 157
    "index-jis0208": 11280,   # max Shift_JIS pointer + 1
    "index-jis0212": 8836,    # 94 * 94
    "index-euc-kr": 23940,    # (0xFE-0x81+1) * 190
}


def download(path: str, url: str) -> None:
    print(f"downloading {url}")
    with urllib.request.urlopen(url, timeout=60) as resp:
        data = resp.read()
    with open(path, "wb") as fh:
        fh.write(data)


def read_index(data_dir: str, base: str) -> list[tuple[int, int]]:
    """Returns [(pointer, code_point), ...] from an index-*.txt file."""
    path = os.path.join(data_dir, base + ".txt")
    if not os.path.exists(path):
        download(path, INDEX_URL.format(base))
    entries = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            parts = stripped.split("\t")
            if len(parts) < 2:
                continue
            pointer = int(parts[0].strip(), 10)
            code_point = int(parts[1].strip(), 16)
            entries.append((pointer, code_point))
    return entries


def read_json(data_dir: str) -> list[dict]:
    path = os.path.join(data_dir, "encodings.json")
    if not os.path.exists(path):
        download(path, ENCODINGS_URL)
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    return [enc for group in data for enc in group["encodings"]]


def emit_u16_table(lines, name, values, per_line=16, ctype=None):
    if ctype is None:
        ctype = "std::uint16_t" if max(values, default=0) <= 0xFFFF else "std::uint32_t"
    lines.append(f"inline constexpr {ctype} {name}[{len(values)}] = {{")
    for i in range(0, len(values), per_line):
        chunk = values[i : i + per_line]
        lines.append("  " + ", ".join(f"0x{v:04X}" for v in chunk) + ",")
    lines.append("};")
    lines.append("")


def emit_ranges(lines: list[str], name: str, values: list[tuple[int, int]]) -> None:
    lines.append(f"inline constexpr RangeEntry {name}[{len(values)}] = {{")
    for pointer, code_point in values:
        lines.append(f"  {{0x{pointer:04X}, 0x{code_point:04X}}},")
    lines.append("};")
    lines.append("")


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    data_dir = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "src", "base", "src"
    )
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    encodings = read_json(data_dir)

    # ------------------------------------------------------------------
    # Labels file.
    # ------------------------------------------------------------------
    label_entries = []
    for enc in encodings:
        name = enc["name"]
        enum = ENUM_NAMES.get(name)
        if enum is None:
            raise SystemExit(f"missing enum mapping for {name!r}")
        for label in enc["labels"]:
            label_entries.append((label.lower(), enum))
    # Keep insertion order for duplicate labels (e.g. none expected), then sort.
    label_entries.sort(key=lambda e: e[0])

    labels_lines = [
        "// Generated by tools/gen_encoding_tables.py - DO NOT EDIT.",
        "#pragma once",
        "#include \"neko/base/encoding.h\"",
        "#include <cstddef>",
        "",
        "namespace neko::base::encoding::detail {",
        "",
        "struct LabelEntry {",
        "  const char* label;",
        "  Charset charset;",
        "};",
        "",
        f"inline constexpr LabelEntry kLabels[{len(label_entries)}] = {{",
    ]
    for label, enum in label_entries:
        labels_lines.append(f'  {{"{label}", Charset::{enum}}},')
    labels_lines.append("};")
    labels_lines.append("")
    labels_lines.append(
        f"inline constexpr std::size_t kLabelCount = {len(label_entries)};"
    )
    labels_lines.append("")
    labels_lines.append("} // namespace neko::base::encoding::detail")
    labels_lines.append("")
    with open(os.path.join(out_dir, "encoding_labels.inc"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(labels_lines))

    # ------------------------------------------------------------------
    # Data file.
    # ------------------------------------------------------------------
    data_lines = [
        "// Generated by tools/gen_encoding_tables.py - DO NOT EDIT.",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace neko::base::encoding::detail {",
        "",
        "// gb18030 four-byte ranges: (pointer, code point) range starts.",
        "struct RangeEntry {",
        "  std::uint32_t pointer;",
        "  std::uint32_t code_point;",
        "};",
        "",
    ]

    # Single-byte tables: index by byte-0x80, 128 entries, 0 = unmapped.
    for enc in encodings:
        name = enc["name"]
        if name not in SINGLE_BYTE_FILES:
            continue
        entries = read_index(data_dir, SINGLE_BYTE_FILES[name])
        table = [0] * 128
        for pointer, code_point in entries:
            assert 0 <= pointer < 128, f"{name}: pointer {pointer} out of range"
            table[pointer] = code_point
        enum = ENUM_NAMES[name]
        emit_u16_table(data_lines, f"kSingleByte{enum[1:]}", table)

    # Multi-byte direct-index tables (0 = unmapped; no multi-byte legacy
    # encoding maps to U+0000, so 0 is a safe sentinel).
    for base, space in POINTER_SPACES.items():
        entries = read_index(data_dir, base)
        table = [0] * space
        for pointer, code_point in entries:
            assert 0 <= pointer < space, f"{base}: pointer {pointer} out of range"
            if table[pointer] != 0:
                raise SystemExit(f"{base}: duplicate pointer {pointer}")
            table[pointer] = code_point
        if base == "index-gb18030":
            emit_u16_table(data_lines, "kGb18030", table)
        elif base == "index-big5":
            emit_u16_table(data_lines, "kBig5", table)
        elif base == "index-jis0208":
            emit_u16_table(data_lines, "kJis0208", table)
        elif base == "index-jis0212":
            emit_u16_table(data_lines, "kJis0212", table)
        elif base == "index-euc-kr":
            emit_u16_table(data_lines, "kEucKr", table)

    ranges = read_index(data_dir, "index-gb18030-ranges")
    emit_ranges(data_lines, "kGb18030Ranges", ranges)
    data_lines.append(
        f"inline constexpr std::size_t kGb18030RangesCount = {len(ranges)};"
    )
    data_lines.append("")

    data_lines.append("} // namespace neko::base::encoding::detail")
    data_lines.append("")
    with open(os.path.join(out_dir, "encoding_data.inc"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(data_lines))

    print(f"wrote {os.path.join(out_dir, 'encoding_labels.inc')}")
    print(f"wrote {os.path.join(out_dir, 'encoding_data.inc')}")


if __name__ == "__main__":
    main()
