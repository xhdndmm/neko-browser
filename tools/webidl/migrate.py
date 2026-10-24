#!/usr/bin/env python3
"""Migrates hand-written prototype registrations into the WebIDL manifest.

This is a one-shot developer tool, not part of the build.  It reads the
``Define<Interface>Prototype()`` functions in ``src/javascript/src/binding``
and moves the *registration* of each member into ``.webidl`` files, deleting
the original registration statements.  Function bodies (the implementation of
the member) stay where they are: the IDL owns the surface, C++ owns the
behaviour.

It only rewrites registrations it fully understands:

    DefineAccessor(ctx, impl.<x>_proto, "name",
                   MakeGetter(ctx, "name", GetFn),
                   MakeSetter(ctx, "name", SetFn));
    DefineGetter(ctx, impl.<x>_proto, "name", MakeGetter(ctx, "name", GetFn));
    MakeGetterMagic / MakeSetterMagic variants;
    a fully regular `static const std::array<...> kMethods = {{...}};` plus the
    `JS_SetPropertyFunctionList` call that consumes it.

Everything else (conditional registration, raw JS_SetPropertyStr, unusual
wrappers) is left untouched and reported, so the migration can never silently
drop a member.

Usage:
    python3 tools/webidl/migrate.py [file ...]   # default: every binding .cpp
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BINDING_DIR = ROOT / "src" / "javascript" / "src" / "binding"
IDL_DIR = ROOT / "src" / "javascript" / "webidl"

FUNC_RE = re.compile(
    r"^[A-Za-z_][\w:<>\*&\s]*?\b(\w+)\s*\(\s*JSContext\*\s*ctx\b[^)]*\)\s*$",
    re.MULTILINE,
)
# Impl's constructor registers the window/global surface; it has no
# JSContext parameter (ctx is a member).
CTOR_RE = re.compile(r"^Impl::Impl\s*\([^;]*\)\s*$", re.MULTILINE)
IMPL_MEMBERS = set(
    re.findall(
        r"^\s+JSValue ([a-z][a-z0-9_]*)\s*=",
        (BINDING_DIR / "binding_internal.h").read_text(),
        re.MULTILINE,
    )
)
CALL_NAMES = ("DefineAccessor", "DefineGetter")
GETTER_RE = re.compile(r'^MakeGetter\(\s*ctx\s*,\s*"([^"]*)"\s*,\s*&?(\w+)\s*\)$')
GETTER_MAGIC_RE = re.compile(
    r'^MakeGetterMagic\(\s*ctx\s*,\s*"([^"]*)"\s*,\s*&?(\w+)\s*,\s*(-?\d+)\s*\)$'
)
SETTER_RE = re.compile(r'^MakeSetter\(\s*ctx\s*,\s*"([^"]*)"\s*,\s*&?(\w+)\s*\)$')
SETTER_MAGIC_RE = re.compile(
    r'^MakeSetterMagic\(\s*ctx\s*,\s*"([^"]*)"\s*,\s*(\w+)\s*,\s*(-?\d+)\s*\)$'
)
METHOD_ARRAY_RE = re.compile(
    r"static\s+const\s+(?:std::array<JSCFunctionListEntry,\s*(\d+)>\s+(\w+)\s*=\s*\{\{"
    r"|JSCFunctionListEntry\s+(\w+)\s*\[\]\s*=\s*\{)"
)
METHOD_ENTRY_RE = re.compile(r'JS_CFUNC_DEF\(\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*(\w+)\s*\)')
CGETSET_ENTRY_RE = re.compile(
    r'JS_CGETSET_DEF\(\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*(?:nullptr|NULL)\s*\)'
)
SET_PROPERTY_LIST_RE_TEMPLATE = (
    r"JS_SetPropertyFunctionList\(\s*ctx\s*,\s*([^,]+?)\s*,\s*"
    r"{name}(?:\.data\(\))?\s*,"
)


# ---------------------------------------------------------------------------
# Source scanning helpers
# ---------------------------------------------------------------------------


def match_brace(text: str, open_index: int) -> int:
    """Index of the '}' matching the '{' at |open_index| (strings/comments aware)."""
    depth = 0
    i = open_index
    while i < len(text):
        c = text[i]
        if c == "/" and i + 1 < len(text) and text[i + 1] == "/":
            i = text.find("\n", i)
            if i < 0:
                return -1
            continue
        if c == "/" and i + 1 < len(text) and text[i + 1] == "*":
            i = text.find("*/", i + 2)
            if i < 0:
                return -1
            i += 2
            continue
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < len(text) and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def match_paren(text: str, open_index: int) -> int:
    depth = 0
    i = open_index
    while i < len(text):
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < len(text) and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def find_calls(body: str, names: tuple[str, ...]):
    """Yields (name, start, open_paren_index) for calls outside strings/comments."""
    pattern = re.compile(r"\b(" + "|".join(names) + r")\s*\(")
    i = 0
    while i < len(body):
        c = body[i]
        if c == "/" and body[i : i + 2] == "//":
            nl = body.find("\n", i)
            i = len(body) if nl < 0 else nl + 1
            continue
        if c == "/" and body[i : i + 2] == "/*":
            end = body.find("*/", i + 2)
            i = len(body) if end < 0 else end + 2
            continue
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < len(body) and body[i] != quote:
                i += 2 if body[i] == "\\" else 1
            i += 1
            continue
        m = pattern.match(body, i)
        if m:
            yield m.group(1), i, m.end() - 1
            i = m.end()
            continue
        i += 1


def split_args(text: str) -> list[str]:
    args: list[str] = []
    depth = 0
    start = 0
    i = 0
    while i < len(text):
        c = text[i]
        if c == '"':
            i += 1
            while i < len(text) and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            args.append(text[start:i].strip())
            start = i + 1
        i += 1
    tail = text[start:].strip()
    if tail:
        args.append(tail)
    return args


def camel(name: str) -> str:
    parts = re.split(r"[^A-Za-z0-9]+", name)
    return "".join(p[:1].upper() + p[1:] for p in parts if p)


def line_of(text: str, index: int) -> int:
    return text.count("\n", 0, index) + 1


# ---------------------------------------------------------------------------
# Extraction
# ---------------------------------------------------------------------------


class Member:
    def __init__(self, kind: str, name: str, getter: str, setter: str | None,
                 magic: str | None, readonly: bool, argc: int = 0):
        self.kind = kind
        self.name = name
        self.getter = getter
        self.setter = setter
        self.magic = magic
        self.readonly = readonly
        self.argc = argc


class FilePlan:
    def __init__(self, path: pathlib.Path):
        self.path = path
        self.text = path.read_text(encoding="utf-8")
        self.removals: list[tuple[int, int]] = []  # [start, end)
        self.groups: dict[str, list[Member]] = {}
        self.insertions: dict[str, list[str]] = {}  # function -> [proto]
        self.skipped: list[str] = []

    def proto_of(self, target: str) -> tuple[str, bool] | None:
        """(name, is_local) for a registration target, or None when unsupported."""
        if target.startswith("impl."):
            field = target[5:]
            if not field.endswith("_proto"):
                return None
            return field, False
        if re.fullmatch(r"[A-Za-z_]\w*", target) and target not in ("ctx", "argv"):
            if target in IMPL_MEMBERS:
                return target, False
            return target, True
        return None

    def extract_function(self, name: str, body_start: int, body_end: int) -> None:
        body = self.text[body_start:body_end]
        base = body_start
        touched: list[str] = []

        # Accessors / getters.
        for call, call_start, open_index in find_calls(body, CALL_NAMES):
            close_index = match_paren(body, open_index)
            if close_index < 0:
                continue
            depth = body.count("{", 0, call_start) - body.count("}", 0, call_start)
            stmt_end = close_index + 1
            while stmt_end < len(body) and body[stmt_end] in " \t":
                stmt_end += 1
            if stmt_end < len(body) and body[stmt_end] == ";":
                stmt_end += 1
            absolute = base + call_start
            if depth != 0:
                self.skipped.append(
                    f"{self.path.name}:{line_of(self.text, absolute)}: {call} inside a "
                    "conditional -- left hand-written"
                )
                continue
            args = split_args(body[open_index + 1 : close_index])
            if len(args) < 4 or args[0] != "ctx":
                self.skipped.append(
                    f"{self.path.name}:{line_of(self.text, absolute)}: {call} has an "
                    "unexpected argument list -- left hand-written"
                )
                continue
            proto = self.proto_of(args[1])
            if proto is None:
                self.skipped.append(
                    f"{self.path.name}:{line_of(self.text, absolute)}: {call} target "
                    f"{args[1]!r} is not impl.<x>_proto or a local JSValue -- left "
                    "hand-written"
                )
                continue
            name_match = re.fullmatch(r'"([^"]+)"', args[2])
            if name_match is None:
                self.skipped.append(
                    f"{self.path.name}:{line_of(self.text, absolute)}: {call} name is "
                    "not a string literal -- left hand-written"
                )
                continue
            member_name = name_match.group(1)

            getter_match = GETTER_RE.match(args[3]) or GETTER_MAGIC_RE.match(args[3])
            if getter_match is None:
                self.skipped.append(
                    f"{self.path.name}:{line_of(self.text, absolute)}: {call} getter is "
                    "not MakeGetter/MakeGetterMagic -- left hand-written"
                )
                continue
            magic = None
            if "Magic" in args[3]:
                magic = GETTER_MAGIC_RE.match(args[3]).group(3)
            if getter_match.group(1) != member_name:
                self.skipped.append(
                    f"{self.path.name}:{line_of(self.text, absolute)}: {call} register "
                    "name differs from the getter name -- left hand-written"
                )
                continue

            setter = None
            if len(args) >= 5:
                setter_match = SETTER_RE.match(args[4]) or SETTER_MAGIC_RE.match(args[4])
                if setter_match is None:
                    self.skipped.append(
                        f"{self.path.name}:{line_of(self.text, absolute)}: {call} "
                        "setter is not MakeSetter/MakeSetterMagic -- left hand-written"
                    )
                    continue
                if setter_match.group(1) != member_name:
                    self.skipped.append(
                        f"{self.path.name}:{line_of(self.text, absolute)}: {call} "
                        "register name differs from the setter name -- left hand-written"
                    )
                    continue
                setter = setter_match.group(2)
            if call == "DefineGetter" and setter is not None:
                self.skipped.append(
                    f"{self.path.name}:{line_of(self.text, absolute)}: DefineGetter with "
                    "a setter -- left hand-written"
                )
                continue

            self.removals.append((absolute, base + stmt_end, name, proto))
            self.groups.setdefault(proto, []).append(
                Member("attribute", member_name, getter_match.group(2), setter, magic,
                       readonly=setter is None)
            )
            touched.append(proto)

        # Fully regular method tables.
        for array_match in METHOD_ARRAY_RE.finditer(body):
            cxx_array = array_match.group(1) is not None
            if cxx_array:
                declared = int(array_match.group(1))
                array_name = array_match.group(2)
                open_token, close_token = "{{", "}};"
            else:
                declared = None
                array_name = array_match.group(3)
                open_token, close_token = "{", "};"
            open_index = body.find(open_token, array_match.start())
            close_index = body.find(close_token, open_index)
            if close_index < 0:
                continue
            array_body = body[open_index + len(open_token) : close_index]
            entries = METHOD_ENTRY_RE.findall(array_body)
            cgetset = CGETSET_ENTRY_RE.findall(array_body)
            residue = METHOD_ENTRY_RE.sub("", array_body)
            residue = CGETSET_ENTRY_RE.sub("", residue)
            residue = re.sub(r"//[^\n]*", "", residue)
            residue = re.sub(r"[\s,]", "", residue)
            if residue or (declared is not None and len(entries) + len(cgetset) != declared):
                self.skipped.append(
                    f"{self.path.name}:{line_of(self.text, base + array_match.start())}: "
                    "method table is not a regular JS_CFUNC_DEF list -- left hand-written"
                )
                continue
            after = body.find(";", close_index) + 1
            set_match = re.compile(
                SET_PROPERTY_LIST_RE_TEMPLATE.format(name=re.escape(array_name))
            ).search(body, after)
            if set_match is None:
                self.skipped.append(
                    f"{self.path.name}:{line_of(self.text, base + array_match.start())}: "
                    "method table has no matching JS_SetPropertyFunctionList -- left "
                    "hand-written"
                )
                continue
            target = set_match.group(1)
            proto = self.proto_of(target)
            if proto is None:
                self.skipped.append(
                    f"{self.path.name}:{line_of(self.text, base + array_match.start())}: "
                    f"method target {target!r} is not impl.<x>_proto -- left hand-written"
                )
                continue
            stmt_end = body.find(";", set_match.end()) + 1
            self.removals.append((base + array_match.start(), base + stmt_end, name, proto))
            self.groups.setdefault(proto, []).extend(
                Member("method", entry[0], entry[2], None, None, readonly=False,
                       argc=int(entry[1]))
                for entry in entries
            )
            self.groups.setdefault(proto, []).extend(
                Member("attribute", entry[0], entry[1], None, None, readonly=True)
                for entry in cgetset
            )
            touched.append(proto)

        for proto in dict.fromkeys(touched):
            self.insertions.setdefault(name, []).append(proto)

    def plan(self) -> None:
        matches = [(m.group(1), m) for m in FUNC_RE.finditer(self.text)]
        matches += [("Impl::Impl", m) for m in CTOR_RE.finditer(self.text)]
        for name, match in matches:
            open_index = self.text.find("{", match.end())
            if open_index < 0:
                continue
            # The constructor's "{" is on the next line; make sure we did not
            # jump into a later function.
            if self.text[match.end() : open_index].count(";") > 0:
                continue
            close_index = match_brace(self.text, open_index)
            if close_index < 0:
                continue
            self.extract_function(name, open_index + 1, close_index)

    def write(self) -> None:
        # Delete the old registration statements, then register through the
        # generated functions immediately after the target prototype is
        # created (not at the end: Headers looks up its own "entries", Abort
        # creates two prototypes at different points).
        text = self.text
        if not self.insertions:
            self.path.write_text(text, encoding="utf-8")
            return

        stem = self.path.stem
        text = text.replace(f'#include "generated/{stem}_generated.inc"\n', "")

        first: dict[tuple[str, tuple[str, bool]], int] = {}
        for start, end, func, proto in self.removals:
            key = (func, proto)
            if key not in first or start < first[key]:
                first[key] = start
        calls = {}
        for (func, proto), at in first.items():
            name = migrated_interface(stem, proto[0])
            receiver = "*this" if func == "Impl::Impl" else "impl"
            calls[(func, proto)] = (
                f"DefineGenerated{name}"
                f"(ctx, {receiver if not proto[1] else proto[0]});"
            )
        for start, end, func, proto in sorted(self.removals, reverse=True):
            key = (func, proto)
            insert_here = first.get(key) == start
            line_start = text.rfind("\n", 0, start) + 1
            indent = text[line_start:start]
            if indent.strip() == "":
                start = line_start
            else:
                indent = "  "
            replacement = ""
            if insert_here and key in calls:
                replacement = f"{indent}{calls.pop(key)}\n"
            text = text[:start] + replacement + text[end:]

        declarations = []
        for protos in self.insertions.values():
            for proto, local in protos:
                name = migrated_interface(stem, proto)
                if local:
                    declarations.append(
                        f"void DefineGenerated{name}(JSContext* ctx, JSValue {proto});"
                    )
                else:
                    declarations.append(
                        f"void DefineGenerated{name}(JSContext* ctx, Impl& impl);"
                    )
        ns = text.find("namespace neko::javascript {")
        if ns != -1:
            insert_at = text.find("\n", ns) + 1
            text = text[:insert_at] + "\n".join(declarations) + "\n\n" + text[insert_at:]

        close = text.rfind("} // namespace neko::javascript")
        if close != -1:
            include = f'#include "generated/{stem}_generated.inc"'
            text = text[:close] + include + "\n\n" + text[close:]
        self.path.write_text(text, encoding="utf-8")

    def write_idl(self) -> None:
        if not self.groups:
            return
        stem = self.path.stem
        blocks = [
            "// Binding surface extracted from "
            f"{self.path.relative_to(ROOT).as_posix()}.",
            "// Function bodies stay hand-written; this file owns the registration.",
            "",
        ]
        for (proto, local), members in self.groups.items():
            blocks.append(f"[Proto={proto}, LocalProto]" if local else f"[Proto={proto}]")
            blocks.append(f"interface {migrated_interface(stem, proto)} {{")
            for member in members:
                if member.kind == "method":
                    params = ", ".join(f"any a{i}" for i in range(member.argc))
                    blocks.append(
                        f"  [CFunction={member.getter}] void {member.name}({params});"
                    )
                else:
                    magic = f", Magic={member.magic}" if member.magic else ""
                    if member.setter is None:
                        blocks.append(
                            f"  [Getter={member.getter}{magic}] readonly attribute any "
                            f"{member.name};"
                        )
                    else:
                        blocks.append(
                            f"  [Getter={member.getter}, Setter={member.setter}{magic}] "
                            f"attribute any {member.name};"
                        )
            blocks.append("};\n")
        IDL_DIR.mkdir(parents=True, exist_ok=True)
        (IDL_DIR / f"{stem}.webidl").write_text("\n".join(blocks), encoding="utf-8")


def migrated_interface(stem: str, proto: str) -> str:
    return camel(stem) + camel(proto)


def main(argv: list[str]) -> int:
    files = [pathlib.Path(arg).resolve() for arg in argv if arg.endswith(".cpp")]
    if not files:
        files = sorted(BINDING_DIR.glob("*_binding.cpp")) + [BINDING_DIR / "impl.cpp"]
    plans = []
    for path in files:
        plan = FilePlan(path)
        plan.plan()
        plans.append(plan)

    for plan in plans:
        plan.write_idl()
        plan.write()

    for plan in plans:
        moved = sum(len(v) for v in plan.groups.values())
        print(f"{plan.path.name}: {moved} members moved to {plan.path.stem}.webidl")
        for note in plan.skipped:
            print(f"  skipped: {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
