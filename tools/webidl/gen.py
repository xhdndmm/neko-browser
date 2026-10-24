#!/usr/bin/env python3
"""WebIDL -> QuickJS binding generator (project subset).

Reads every ``*.webidl`` file under ``src/javascript/webidl`` and writes the
matching ``*_generated.{h,cpp}`` under
``src/javascript/src/binding/generated``.

The generator is deliberately small: it implements the slice of WebIDL the
project actually uses today, plus the extended attributes that tell it how a
member maps onto the engine.  It refuses anything it cannot emit correctly
instead of guessing -- an unsupported type is a hard error, not a stub.

Supported subset
----------------

    [Exposed=Window]
    interface HTMLElement : Element {
      [Reflect=title] attribute DOMString title;
      [Reflect=inert] attribute boolean inert;
      [CFunction=ElementFocus] void focus();
    };

* one interface per file (the file name selects the output file);
* members: ``attribute`` (optionally ``readonly``) and methods;
* types: ``boolean`` and ``DOMString`` for reflected attributes; methods may
  use any return type because their body is hand-written;
* extended attributes:
    - ``[Reflect=<content attribute>]`` -- the generator emits the getter and
      setter that reflect the IDL attribute to that content attribute and mark
      the DOM dirty.  Only ``boolean`` and ``DOMString`` are supported.
    - ``[CFunction=<C++ symbol>]`` -- a method is registered by name against an
      existing C++ function (declared in binding_internal.h).
    - ``[Getter=<symbol>]`` / ``[Setter=<symbol>]`` -- register an existing
      hand-written accessor pair (no body is generated).

Anything else is an error.  Interfaces inherit nothing at code-generation time:
members declared here are registered on the protocol passed to the generated
``DefineGenerated<Interface>()`` function, which the hand-written
``Define<Interface>Prototype()`` calls in the right place.

Usage
-----

    python3 tools/webidl/gen.py            # regenerate
    python3 tools/webidl/gen.py --check    # verify the checked-in files match
"""

from __future__ import annotations

import dataclasses
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
IDL_DIR = ROOT / "src" / "javascript" / "webidl"
OUT_DIR = ROOT / "src" / "javascript" / "src" / "binding" / "generated"

HEADER = """// GENERATED FILE -- DO NOT EDIT.
// Source: {source}
// Generator: tools/webidl/gen.py (WebIDL subset; see that file for the grammar).
// Regenerate: python3 tools/webidl/gen.py
"""


# ---------------------------------------------------------------------------
# AST
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class ExtendedAttr:
    name: str
    value: str | None
    line: int


@dataclasses.dataclass
class Member:
    kind: str  # "attribute" | "method"
    name: str
    type: str
    extended: dict[str, ExtendedAttr]
    line: int
    readonly: bool = False
    arg_count: int = 0


@dataclasses.dataclass
class Interface:
    name: str
    base: str | None
    members: list[Member]
    line: int


class IdlError(Exception):
    pass


# ---------------------------------------------------------------------------
# Lexer
# ---------------------------------------------------------------------------

_TOKEN_RE = re.compile(
    r"""
      (?P<space>\s+)
    | (?P<comment>//[^\n]*|/\*.*?\*/)
    | (?P<ident>[A-Za-z_][A-Za-z0-9_-]*)
    | (?P<punct>[{}()\[\];:,?=<>])
    | (?P<other>.)
    """,
    re.VERBOSE | re.DOTALL,
)


def tokenize(text: str, source: str) -> list[tuple[str, str, int]]:
    tokens: list[tuple[str, str, int]] = []
    line = 1
    for match in _TOKEN_RE.finditer(text):
        kind = match.lastgroup
        value = match.group()
        if kind in ("space", "comment"):
            line += value.count("\n")
            continue
        if kind == "other":
            raise IdlError(f"{source}:{line}: unexpected character {value!r}")
        tokens.append((kind, value, line))
        line += value.count("\n")
    tokens.append(("eof", "", line))
    return tokens


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------


class Parser:
    def __init__(self, tokens: list[tuple[str, str, int]], source: str):
        self.tokens = tokens
        self.pos = 0
        self.source = source

    def peek(self) -> tuple[str, str, int]:
        return self.tokens[self.pos]

    def next(self) -> tuple[str, str, int]:
        token = self.tokens[self.pos]
        self.pos += 1
        return token

    def error(self, message: str) -> IdlError:
        _, value, line = self.peek()
        near = value if value else "end of file"
        return IdlError(f"{self.source}:{line}: {message} (near {near!r})")

    def expect(self, value: str) -> tuple[str, str, int]:
        token = self.next()
        if token[1] != value:
            self.pos -= 1
            raise self.error(f"expected {value!r}")
        return token

    def accept(self, value: str) -> bool:
        if self.peek()[1] == value:
            self.pos += 1
            return True
        return False

    def parse_extended_attrs(self) -> dict[str, ExtendedAttr]:
        attrs: dict[str, ExtendedAttr] = {}
        if not self.accept("["):
            return attrs
        while True:
            _, name, line = self.next()
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
                raise self.error("expected an extended attribute name")
            value = None
            if self.accept("="):
                if self.peek()[0] == "ident":
                    value = self.next()[1]
                elif self.peek()[1] == '"':
                    value = self.parse_string()
                else:
                    raise self.error("expected an extended attribute value")
            attrs[name] = ExtendedAttr(name, value, line)
            if self.accept(","):
                continue
            self.expect("]")
            return attrs

    def parse_string(self) -> str:
        self.expect('"')
        out = []
        for kind, value, _ in self.tokens[self.pos :]:
            self.next()
            if value == '"':
                return "".join(out)
            out.append(value)
        raise self.error("unterminated string")

    def parse(self) -> Interface:
        self.parse_extended_attrs()  # [Exposed=Window] -- informational
        self.expect("interface")
        _, name, line = self.next()
        base = None
        if self.accept(":"):
            base = self.next()[1]
        self.expect("{")
        members: list[Member] = []
        while not self.accept("}"):
            members.append(self.parse_member())
        self.expect(";")
        if self.peek()[0] != "eof":
            raise self.error("trailing content after the interface")
        return Interface(name, base, members, line)

    def parse_member(self) -> Member:
        extended = self.parse_extended_attrs()
        readonly = self.accept("readonly")
        if self.accept("attribute"):
            _, type_name, line = self.next()
            _, name, _ = self.next()
            self.expect(";")
            if readonly:
                # A readonly attribute is written like an accessor pair with a
                # getter only; [Reflect] cannot express "no setter" yet.
                if "Reflect" in extended:
                    raise IdlError(
                        f"{self.source}:{line}: [Reflect] on a readonly attribute is "
                        "not supported yet; use [Getter=...]"
                    )
            return Member("attribute", name, type_name, extended, line, readonly)

        # Method: <return type> <name> "(" params? ")" ";"
        _, return_type, line = self.next()
        _, name, _ = self.next()
        self.expect("(")
        argc = 0
        if not self.accept(")"):
            while True:
                self.accept("optional")
                self.next()  # parameter type
                self.next()  # parameter name
                argc += 1
                if self.accept(","):
                    continue
                self.expect(")")
                break
        self.expect(";")
        member = Member("method", name, return_type, extended, line, arg_count=argc)
        return member


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

_REFLECT_TYPES = {"boolean", "DOMString"}


def validate(iface: Interface, source: str) -> None:
    for member in iface.members:
        where = f"{source}:{member.line}"
        if member.kind == "attribute":
            has_reflect = "Reflect" in member.extended
            has_accessors = "Getter" in member.extended or "Setter" in member.extended
            if has_reflect and has_accessors:
                raise IdlError(f"{where}: use [Reflect] or [Getter]/[Setter], not both")
            if has_reflect:
                if member.type not in _REFLECT_TYPES:
                    raise IdlError(
                        f"{where}: [Reflect] supports boolean and DOMString, "
                        f"not {member.type!r}"
                    )
                if not member.extended["Reflect"].value:
                    raise IdlError(f"{where}: [Reflect] needs a content attribute name")
                continue
            if has_accessors:
                if not member.readonly and "Setter" not in member.extended:
                    raise IdlError(f"{where}: writable attribute needs [Setter=...]")
                continue
            raise IdlError(
                f"{where}: attribute {member.name!r} needs [Reflect=...] or "
                "[Getter=...]/[Setter=...]"
            )
        else:
            if "CFunction" not in member.extended:
                raise IdlError(f"{where}: method {member.name!r} needs [CFunction=...]")


# ---------------------------------------------------------------------------
# C++ emitter
# ---------------------------------------------------------------------------

_CPP_KEYWORDS = {
    "class",
    "delete",
    "new",
    "namespace",
    "operator",
    "template",
    "this",
    "default",
    "friend",
}


def cpp_ident(name: str) -> str:
    parts = re.split(r"[^A-Za-z0-9]+", name)
    out = "".join(part[:1].upper() + part[1:] for part in parts if part)
    if not out:
        raise IdlError(f"cannot derive a C++ identifier from {name!r}")
    if out[0].isdigit():
        out = "_" + out
    return out


def symbol_suffix(member: Member) -> str:
    """A stable, collision-free suffix for one member's generated symbols."""
    return cpp_ident(member.name)


def emit_getter(iface: Interface, member: Member) -> str:
    attr = member.extended["Reflect"].value
    suffix = symbol_suffix(member)
    fn = f"Generated{iface.name}Get{suffix}"
    if member.type == "boolean":
        body = f"""JSValue {fn}(JSContext* ctx, JSValueConst this_val)
{{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {{
    return JS_ThrowTypeError(ctx, "not an element");
  }}
  return JS_NewBool(ctx, element->HasAttribute("{attr}"));
}}"""
    else:
        body = f"""JSValue {fn}(JSContext* ctx, JSValueConst this_val)
{{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {{
    return JS_ThrowTypeError(ctx, "not an element");
  }}
  const std::optional<std::string_view> value = element->GetAttribute("{attr}");
  return value.has_value() ? JS_NewStringLen(ctx, value->data(), value->size())
                           : JS_NewStringLen(ctx, "", 0);
}}"""
    return body


def emit_setter(iface: Interface, member: Member) -> str:
    attr = member.extended["Reflect"].value
    suffix = symbol_suffix(member)
    fn = f"Generated{iface.name}Set{suffix}"
    if member.type == "boolean":
        body = f"""JSValue {fn}(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {{
    return JS_ThrowTypeError(ctx, "not an element");
  }}
  const int flag = JS_ToBool(ctx, value);
  if (flag < 0) {{
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_EXCEPTION;
  }}
  if (flag != 0) {{
    element->SetAttribute("{attr}", "");
  }} else {{
    element->RemoveAttribute("{attr}");
  }}
  ImplFor(ctx, this_val)->MarkAttrDirty(element, "{attr}");
  return JS_UNDEFINED;
}}"""
    else:
        body = f"""JSValue {fn}(JSContext* ctx, JSValueConst this_val, JSValueConst value)
{{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {{
    return JS_ThrowTypeError(ctx, "not an element");
  }}
  bool ok = false;
  const std::string text = ArgString(ctx, value, &ok);
  if (!ok) {{
    return JS_EXCEPTION;
  }}
  if (text.empty()) {{
    element->RemoveAttribute("{attr}");
  }} else {{
    element->SetAttribute("{attr}", text);
  }}
  ImplFor(ctx, this_val)->MarkAttrDirty(element, "{attr}");
  return JS_UNDEFINED;
}}"""
    return body


def emit_source(iface: Interface, source: str, stem: str) -> str:
    name = iface.name
    out: list[str] = [HEADER.format(source=source)]
    out.append(f'#include "{stem}_generated.h"\n')
    out.append('#include "../binding_internal.h"\n')
    out.append("#include <iterator>")
    out.append("#include <optional>")
    out.append("#include <string>")
    out.append("#include <string_view>\n")
    out.append("namespace neko::javascript {")
    out.append("namespace {\n")

    for member in iface.members:
        if member.kind != "attribute" or "Reflect" not in member.extended:
            continue
        out.append(f"// [Reflect={member.extended['Reflect'].value}] "
                   f"{'readonly ' if member.readonly else ''}attribute "
                   f"{member.type} {member.name};")
        out.append(emit_getter(iface, member))
        out.append("")
        if not member.readonly:
            out.append(emit_setter(iface, member))
            out.append("")

    methods = [m for m in iface.members if m.kind == "method"]
    if methods:
        out.append("// Methods: the body is hand-written; the generator owns the")
        out.append("// registration so the name and argument count cannot drift.")
        out.append("constexpr JSCFunctionListEntry kMethods[] = {")
        for member in methods:
            c_function = member.extended["CFunction"].value
            out.append(
                f'    JS_CFUNC_DEF("{member.name}", {member.arg_count}, {c_function}),'
            )
        out.append("};\n")

    out.append("} // namespace\n")
    out.append(f"void DefineGenerated{name}(JSContext* ctx, JSValue proto)")
    out.append("{")
    if methods:
        out.append("  JS_SetPropertyFunctionList(")
        out.append("      ctx, proto, kMethods, static_cast<int>(std::size(kMethods)));")
    for member in iface.members:
        if member.kind != "attribute":
            continue
        suffix = symbol_suffix(member)
        getter = f"Generated{name}Get{suffix}"
        if "Reflect" in member.extended:
            setter = f"Generated{name}Set{suffix}"
            out.append(
                f'  DefineAccessor(ctx, proto, "{member.name}", '
                f'MakeGetter(ctx, "{member.name}", {getter}), '
                f'MakeSetter(ctx, "{member.name}", {setter}));'
            )
        else:
            getter_sym = member.extended["Getter"].value
            if member.readonly:
                out.append(
                    f'  DefineGetter(ctx, proto, "{member.name}", '
                    f'MakeGetter(ctx, "{member.name}", {getter_sym}));'
                )
            else:
                setter_sym = member.extended["Setter"].value
                out.append(
                    f'  DefineAccessor(ctx, proto, "{member.name}", '
                    f'MakeGetter(ctx, "{member.name}", {getter_sym}), '
                    f'MakeSetter(ctx, "{member.name}", {setter_sym}));'
                )
    out.append("}\n")
    out.append("} // namespace neko::javascript")
    return "\n".join(out) + "\n"


def emit_header(iface: Interface, source: str) -> str:
    return (
        HEADER.format(source=source)
        + "\n#pragma once\n\n#include <quickjs.h>\n\n"
        + "namespace neko::javascript {\n\n"
        + f"// Registers the members declared in {source} on |proto|.\n"
        + f"void DefineGenerated{iface.name}(JSContext* ctx, JSValue proto);\n\n"
        + "} // namespace neko::javascript\n"
    )


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------


def format_cpp(text: str, filename: pathlib.Path) -> str:
    """Runs clang-format so the checked-in output is exactly what CI checks.

    The project's formatting gate covers src/, so generated files must be
    clang-format clean too.  CLANG_FORMAT overrides the binary (same variable
    tools/format.sh uses).
    """
    binary = os.environ.get("CLANG_FORMAT", "clang-format")
    try:
        proc = subprocess.run(
            [binary, f"-assume-filename={filename}", "-style=file"],
            input=text,
            text=True,
            capture_output=True,
            cwd=ROOT,
            check=False,
        )
    except FileNotFoundError:
        raise IdlError(
            "clang-format not found; install it or set CLANG_FORMAT "
            "(the generated C++ must be formatted)"
        )
    if proc.returncode != 0:
        raise IdlError(f"clang-format failed on {filename.name}: {proc.stderr.strip()}")
    return proc.stdout


def generate() -> dict[pathlib.Path, str]:
    outputs: dict[pathlib.Path, str] = {}
    sources = sorted(IDL_DIR.glob("*.webidl"))
    if not sources:
        raise IdlError(f"no .webidl files under {IDL_DIR}")
    for path in sources:
        source = path.relative_to(ROOT).as_posix()
        text = path.read_text(encoding="utf-8")
        iface = Parser(tokenize(text, source), source).parse()
        validate(iface, source)
        stem = path.stem
        header_path = OUT_DIR / f"{stem}_generated.h"
        source_path = OUT_DIR / f"{stem}_generated.cpp"
        outputs[header_path] = format_cpp(emit_header(iface, source), header_path)
        outputs[source_path] = format_cpp(emit_source(iface, source, stem), source_path)
    return outputs


def main(argv: list[str]) -> int:
    check = "--check" in argv
    try:
        outputs = generate()
    except IdlError as error:
        print(f"webidl: {error}", file=sys.stderr)
        return 1

    stale: list[str] = []
    for path, text in sorted(outputs.items()):
        if check:
            current = path.read_text(encoding="utf-8") if path.exists() else ""
            if current != text:
                stale.append(str(path.relative_to(ROOT)))
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        print(f"wrote {path.relative_to(ROOT)}")

    if check:
        if stale:
            print(
                "webidl: generated sources are out of date:\n  "
                + "\n  ".join(stale)
                + "\nrun: python3 tools/webidl/gen.py",
                file=sys.stderr,
            )
            return 1
        print("webidl: generated sources are up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
