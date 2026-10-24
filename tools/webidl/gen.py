#!/usr/bin/env python3
"""WebIDL -> QuickJS binding generator (project subset).

Reads every ``*.webidl`` file under ``src/javascript/webidl`` and writes the
matching ``*_generated.inc`` under ``src/javascript/src/binding/generated``.

The output is an ``.inc`` included *inside* the hand-written translation unit:
binding member implementations are file-local (anonymous namespace), so a
separate TU cannot reference them.

The generator is deliberately small: it implements the slice of WebIDL the
project actually uses today, plus the extended attributes that tell it how a
member maps onto the engine.  It refuses anything it cannot emit correctly
instead of guessing -- an unsupported type is a hard error, not a stub.

Grammar (subset)
----------------

    file      := interface*
    interface := ExtendedAttrs "interface" Ident (":" Ident)? "{" member* "}" ";"
    member    := ExtendedAttrs "readonly"? "attribute" Type Ident ";"
               | ExtendedAttrs Type Ident "(" params? ")" ";"

Extended attributes
-------------------

Interface:

* ``[Proto=<impl field>]`` -- **required**; the prototype every member of this
  interface is registered on (e.g. ``element_proto``).  The generated function
  takes the binder's ``Impl&`` and reads ``impl.<field>``.
* ``[Exposed=...]`` is accepted and ignored.

Members:

* ``[Reflect=<content attribute>]`` -- the generator emits the getter and
  setter that reflect the IDL attribute to a content attribute and mark the DOM
  dirty.  Only ``boolean`` and ``DOMString`` are supported.
* ``[Getter=<symbol>]`` / ``[Setter=<symbol>]`` -- register existing
  hand-written accessors; no body is generated.  ``readonly`` means getter
  only.
* ``[Magic=<int>]`` -- with Getter/Setter, use ``MakeGetterMagic`` /
  ``MakeSetterMagic``.
* ``[CFunction=<symbol>]`` -- a method is registered by name against an
  existing C++ function (declared in binding_internal.h).

Generated API
-------------

For every interface, the header declares::

    void DefineGenerated<Interface>(JSContext* ctx, Impl& impl);

which registers the interface's members on ``impl.<Proto>``.

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
    extended: dict[str, ExtendedAttr]
    line: int

    @property
    def proto(self) -> str:
        attr = self.extended.get("Proto")
        if attr is None or not attr.value:
            raise IdlError(
                f"interface {self.name} needs [Proto=<impl field>] "
                f"(line {self.line})"
            )
        return attr.value

    @property
    def local_proto(self) -> bool:
        """True when [Proto] names a local JSValue, not an Impl field."""
        return "LocalProto" in self.extended


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
                else:
                    raise self.error("expected an extended attribute value")
            attrs[name] = ExtendedAttr(name, value, line)
            if self.accept(","):
                continue
            self.expect("]")
            return attrs

    def parse(self) -> list[Interface]:
        interfaces: list[Interface] = []
        while self.peek()[0] != "eof":
            interfaces.append(self.parse_interface())
        return interfaces

    def parse_interface(self) -> Interface:
        extended = self.parse_extended_attrs()
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
        return Interface(name, base, members, extended, line)

    def parse_member(self) -> Member:
        extended = self.parse_extended_attrs()
        readonly = self.accept("readonly")
        if self.accept("attribute"):
            _, type_name, line = self.next()
            _, name, _ = self.next()
            self.expect(";")
            return Member("attribute", name, type_name, extended, line, readonly)

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
        return Member("method", name, return_type, extended, line, arg_count=argc)


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

_REFLECT_TYPES = {"boolean", "DOMString"}


def validate(iface: Interface, source: str) -> None:
    iface.proto  # raises when missing
    for member in iface.members:
        where = f"{source}:{member.line}"
        if member.kind == "attribute":
            has_reflect = "Reflect" in member.extended
            has_getter = "Getter" in member.extended
            has_setter = "Setter" in member.extended
            if has_reflect and (has_getter or has_setter):
                raise IdlError(f"{where}: use [Reflect] or [Getter]/[Setter], not both")
            if has_reflect:
                if member.type not in _REFLECT_TYPES:
                    raise IdlError(
                        f"{where}: [Reflect] supports boolean and DOMString, "
                        f"not {member.type!r}"
                    )
                if not member.extended["Reflect"].value:
                    raise IdlError(f"{where}: [Reflect] needs a content attribute name")
                if member.readonly:
                    raise IdlError(f"{where}: [Reflect] on a readonly attribute is not supported")
                continue
            if has_getter:
                if not member.readonly and not has_setter:
                    raise IdlError(f"{where}: writable attribute needs [Setter=...]")
                if "Magic" in member.extended and not member.extended["Magic"].value:
                    raise IdlError(f"{where}: [Magic] needs an integer")
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


def cpp_ident(name: str) -> str:
    parts = re.split(r"[^A-Za-z0-9]+", name)
    out = "".join(part[:1].upper() + part[1:] for part in parts if part)
    if not out:
        raise IdlError(f"cannot derive a C++ identifier from {name!r}")
    if out[0].isdigit():
        out = "_" + out
    return out


def member_suffix(member: Member) -> str:
    return cpp_ident(member.name)


def emit_reflect_getter(iface: Interface, member: Member) -> str:
    attr = member.extended["Reflect"].value
    fn = f"Generated{iface.name}Get{member_suffix(member)}"
    if member.type == "boolean":
        return f"""JSValue {fn}(JSContext* ctx, JSValueConst this_val)
{{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {{
    return JS_ThrowTypeError(ctx, "not an element");
  }}
  return JS_NewBool(ctx, element->HasAttribute("{attr}"));
}}"""
    return f"""JSValue {fn}(JSContext* ctx, JSValueConst this_val)
{{
  dom::Element* element = AsElement(UnwrapNode(ctx, this_val));
  if (element == nullptr) {{
    return JS_ThrowTypeError(ctx, "not an element");
  }}
  const std::optional<std::string_view> value = element->GetAttribute("{attr}");
  return value.has_value() ? JS_NewStringLen(ctx, value->data(), value->size())
                           : JS_NewStringLen(ctx, "", 0);
}}"""


def emit_reflect_setter(iface: Interface, member: Member) -> str:
    attr = member.extended["Reflect"].value
    fn = f"Generated{iface.name}Set{member_suffix(member)}"
    if member.type == "boolean":
        return f"""JSValue {fn}(JSContext* ctx, JSValueConst this_val, JSValueConst value)
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
    return f"""JSValue {fn}(JSContext* ctx, JSValueConst this_val, JSValueConst value)
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


def emit_source(interfaces: list[Interface], source: str, stem: str) -> str:
    out: list[str] = [HEADER.format(source=source)]
    out.append("// Included by the hand-written binding file, inside its namespace:")
    out.append("// the member implementations are file-local and must stay visible.")
    out.append("")
    out.append("#include <iterator>")
    out.append("#include <optional>")
    out.append("#include <string>")
    out.append("#include <string_view>\n")

    emitted_helpers = False
    for iface in interfaces:
        helpers = [
            m
            for m in iface.members
            if m.kind == "attribute" and "Reflect" in m.extended
        ]
        if not helpers:
            continue
        if not emitted_helpers:
            out.append("namespace {\n")
            emitted_helpers = True
        for member in helpers:
            out.append(f"// [Reflect={member.extended['Reflect'].value}] "
                       f"attribute {member.type} {member.name};")
            out.append(emit_reflect_getter(iface, member))
            out.append("")
            out.append(emit_reflect_setter(iface, member))
            out.append("")
    if emitted_helpers:
        out.append("} // namespace\n")

    for iface in interfaces:
        methods = [m for m in iface.members if m.kind == "method"]
        if methods:
            out.append(f"namespace {{\n")
            out.append(f"constexpr JSCFunctionListEntry k{iface.name}Methods[] = {{")
            for member in methods:
                out.append(
                    f'    JS_CFUNC_DEF("{member.name}", {member.arg_count}, '
                    f'{member.extended["CFunction"].value}),'
                )
            out.append("};\n")
            out.append("} // namespace\n")

        if iface.local_proto:
            out.append(
                f"void DefineGenerated{iface.name}(JSContext* ctx, JSValue {iface.proto})"
            )
            proto = iface.proto
        else:
            out.append(f"void DefineGenerated{iface.name}(JSContext* ctx, Impl& impl)")
            proto = f"impl.{iface.proto}"
        out.append("{")
        if methods:
            out.append(f"  JS_SetPropertyFunctionList(")
            out.append(f"      ctx, {proto}, k{iface.name}Methods,")
            out.append(f"      static_cast<int>(std::size(k{iface.name}Methods)));")
        for member in iface.members:
            if member.kind != "attribute":
                continue
            if "Reflect" in member.extended:
                getter = f"Generated{iface.name}Get{member_suffix(member)}"
                setter = f"Generated{iface.name}Set{member_suffix(member)}"
            else:
                getter = member.extended["Getter"].value
                setter = member.extended["Setter"].value if "Setter" in member.extended else None
            magic = member.extended.get("Magic")
            if magic is not None:
                getter_expr = (
                    f'MakeGetterMagic(ctx, "{member.name}", {getter}, {magic.value})'
                )
                setter_expr = (
                    f'MakeSetterMagic(ctx, "{member.name}", {setter}, {magic.value})'
                    if setter
                    else None
                )
            else:
                getter_expr = f'MakeGetter(ctx, "{member.name}", {getter})'
                setter_expr = f'MakeSetter(ctx, "{member.name}", {setter})' if setter else None
            if setter_expr is None:
                out.append(
                    f'  DefineGetter(ctx, {proto}, "{member.name}", {getter_expr});'
                )
            else:
                out.append(
                    f'  DefineAccessor(ctx, {proto}, "{member.name}", {getter_expr}, '
                    f"{setter_expr});"
                )
        out.append("}\n")

    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------


def format_cpp(text: str, filename: pathlib.Path) -> str:
    """Runs clang-format so the checked-in output is exactly what CI checks."""
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
        interfaces = Parser(tokenize(text, source), source).parse()
        for iface in interfaces:
            validate(iface, source)
        stem = path.stem
        source_path = OUT_DIR / f"{stem}_generated.inc"
        outputs[source_path] = format_cpp(
            emit_source(interfaces, source, stem), source_path
        )
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
