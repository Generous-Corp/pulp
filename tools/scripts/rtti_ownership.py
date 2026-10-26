#!/usr/bin/env python3
"""Which classes an archive owns, and which RTTI another archive copied.

A library that carries its own weak typeinfo, vtable or typeinfo name for a
class was compiled against that class's definition, even when the weak copy
resolves every reference it makes. The archive symbol closure check uses this
to flag such copies of a forbidden archive's classes.

Ownership is read from the symbols alone: an archive owns class X when it
strongly defines a member of X that is not RTTI (a member function, a static
data member, a thunk into one). An inline-only class (every member inline, so
every symbol weak) is owned by nobody here and is never flagged; that keeps
the check from guessing.

Never flagged, whatever owns them:

* anything in std:: (std::function's __func bases, std::format_error, ...),
  which every user of the standard library emits;
* closure types (lambdas, blocks, unnamed types), which belong to the
  function that contains them, not to a class anyone could link against.

Demangling uses c++filt or llvm-cxxfilt, after dropping Mach-O's extra
leading underscore, so the same rule reads Mach-O and ELF names.
"""

from __future__ import annotations

import re
import shutil
import subprocess

# Itanium-ABI RTTI and its relatives: typeinfo (_ZTI), typeinfo name (_ZTS),
# vtable (_ZTV), VTT (_ZTT) and construction vtable (_ZTC).
RTTI_MANGLED = re.compile(r"^_?_ZT[IVSTC]")
_RTTI_DEMANGLED = re.compile(
    r"^(?:typeinfo name for|typeinfo for|vtable for|VTT for|construction vtable for) (?P<type>.+)$")
# An operator's own name: brackets and spaces inside it ("operator>",
# "operator()", "operator bool", "operator new[]") are not nesting or a return
# type, so member_owner masks it before measuring depth.
_OPERATOR_NAME = re.compile(
    r"\boperator(?:\s*(?:new|delete)(?:\s*\[\])?(?![\w])"
    r"|\s*\(\)|\s*\[\]|\s*->\*?|\s*<=>|\s*(?:<<|>>)=?"
    r"|\s*(?:&&|\|\||\+\+|--|[-+*/%^&|~!=<>,]=?)"
    r"|\s*\"\"\s*\w+"
    r"|\s+[^()]+?(?=\())")
_CLOSURE = re.compile(r"'lambda|\{lambda|'block|\{block|'unnamed|\{unnamed|\$_\d|\(anonymous namespace\)")


class DemangleError(RuntimeError):
    pass


def demangle(names: set[str], cxxfilt: str | None = None) -> dict[str, str]:
    """Mangled name -> demangled text, one to one."""
    if not names:
        return {}
    tool = cxxfilt or shutil.which("c++filt") or shutil.which("llvm-cxxfilt")
    if not tool:
        raise DemangleError("c++filt (or llvm-cxxfilt) is required to read RTTI ownership")
    ordered = sorted(names)
    # Mach-O names carry one extra leading underscore; it is removed here, so
    # the tool must never strip another. Older Apple c++filt (the Xcode 16 on
    # GitHub-hosted macOS) strips one by default on Darwin, which leaves an
    # Itanium "_Z..." name undemangled and every RTTI owner unread. -n
    # (--no-strip-underscore) turns that off in llvm-cxxfilt and GNU c++filt.
    itanium = [name[1:] if name.startswith("__Z") else name for name in ordered]
    proc = subprocess.run([tool, "-n"], input="\n".join(itanium), capture_output=True, text=True,
                          encoding="utf-8", errors="replace", check=False)
    out = proc.stdout.splitlines()
    if proc.returncode != 0 or len(out) != len(ordered):
        raise DemangleError(f"{tool} did not demangle {len(ordered)} names one-to-one")
    return dict(zip(ordered, out))


def _top_level(text: str, stop: str) -> int:
    """Index of the first `stop` character outside <> and () nesting, or -1."""
    depth = 0
    for index, char in enumerate(text):
        if char in "<(" and depth == 0 and char == stop:
            return index
        if char in "<(":
            depth += 1
        elif char in ">)":
            depth -= 1
    return -1


def _scopes(qualified: str) -> list[str]:
    parts, current, depth, index = [], "", 0, 0
    while index < len(qualified):
        char = qualified[index]
        if char in "<(":
            depth += 1
        elif char in ">)":
            depth -= 1
        if depth == 0 and qualified.startswith("::", index):
            parts.append(current)
            current = ""
            index += 2
            continue
        current += char
        index += 1
    parts.append(current)
    return parts


def member_owner(demangled: str) -> str | None:
    """The class or namespace a demangled member belongs to, or None.

    "pulp::view::Meter::set_level(float, float)" -> "pulp::view::Meter". A free
    function yields its namespace, which matches no RTTI and is harmless.
    """
    if _RTTI_DEMANGLED.match(demangled):
        return None
    demangled = _OPERATOR_NAME.sub("operator_", demangled)
    paren = _top_level(demangled, "(")
    name = demangled[:paren] if paren >= 0 else demangled
    # Keep the last top-level word: that drops a template instantiation's
    # return type and the wording of special names ("non-virtual thunk to",
    # "guard variable for", "TLS init function for", ...), which all end in
    # the member they are about.
    depth, start = 0, 0
    for index, char in enumerate(name):
        if char in "<(":
            depth += 1
        elif char in ">)":
            depth -= 1
        elif char == " " and depth == 0:
            start = index + 1
    parts = _scopes(name[start:].strip())
    return "::".join(parts[:-1]) if len(parts) > 1 else None


def rtti_class(demangled: str) -> str | None:
    """The class an RTTI symbol describes ("vtable for X" -> "X")."""
    match = _RTTI_DEMANGLED.match(demangled)
    if not match:
        return None
    subject = match["type"]
    if demangled.startswith("construction vtable for "):
        # "construction vtable for Base-in-Derived": Derived is being built.
        subject = subject.rsplit("-in-", 1)[-1]
    return subject


def never_flagged(cls: str) -> bool:
    return cls.startswith("std::") or bool(_CLOSURE.search(cls))


def owned_classes(strong: set[str], names: dict[str, str]) -> set[str]:
    """Classes whose non-RTTI members `strong` defines."""
    owned = set()
    for symbol in strong:
        if RTTI_MANGLED.match(symbol):
            continue
        owner = member_owner(names[symbol])
        if owner:
            owned.add(owner)
    return owned


def copied_rtti(weak: set[str], owned: set[str], names: dict[str, str]) -> dict[str, str]:
    """Weak-only RTTI symbols for an owned class: mangled -> demangled."""
    hits = {}
    for symbol in weak:
        if not RTTI_MANGLED.match(symbol):
            continue
        cls = rtti_class(names[symbol])
        if cls and cls in owned and not never_flagged(cls):
            hits[symbol] = names[symbol]
    return hits
