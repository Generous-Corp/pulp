#!/usr/bin/env python3
"""Node ABI virtual declaration gate.

Processor and PluginSlot are SDK-facing polymorphic surfaces. Within a major
node ABI version, new virtual methods must be appended at the end of the class
vtable. Inserting, removing, reordering, or re-signaturing existing virtuals
changes the node contract.

The gate compares the current working tree against a git base and requires the
base virtual-method declarations to remain a prefix of the current order.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import re
import subprocess
import sys
from pathlib import Path


SURFACES = (
    (
        "Processor",
        "core/format/include/pulp/format/processor.hpp",
    ),
    (
        "PluginSlot",
        "core/host/include/pulp/host/plugin_slot.hpp",
    ),
)


@dataclass(frozen=True)
class VirtualDecl:
    name: str
    signature: str


def repo_root() -> Path | None:
    result = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    return Path(result.stdout.strip())


def git_show(base: str, rel_path: str) -> str | None:
    result = subprocess.run(
        ["git", "show", f"{base}:{rel_path}"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    return result.stdout


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//.*", "", text)


def class_body(text: str, class_name: str) -> str:
    match = re.search(rf"\bclass\s+{re.escape(class_name)}\b[^{{]*{{", text)
    if not match:
        raise ValueError(f"class {class_name} not found")

    start = match.end()
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    if depth != 0:
        raise ValueError(f"class {class_name} body is unbalanced")
    return text[start:i - 1]


def _matching_paren(text: str, open_index: int) -> int:
    depth = 1
    i = open_index + 1
    while i < len(text) and depth > 0:
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
        i += 1
    if depth != 0:
        raise ValueError("virtual method declaration has unbalanced parentheses")
    return i - 1


def _split_params(params: str) -> list[str]:
    if not params.strip():
        return []
    out: list[str] = []
    start = 0
    angle_depth = 0
    paren_depth = 0
    for i, ch in enumerate(params):
        if ch == "<":
            angle_depth += 1
        elif ch == ">" and angle_depth > 0:
            angle_depth -= 1
        elif ch == "(":
            paren_depth += 1
        elif ch == ")" and paren_depth > 0:
            paren_depth -= 1
        elif ch == "," and angle_depth == 0 and paren_depth == 0:
            out.append(params[start:i])
            start = i + 1
    out.append(params[start:])
    return out


def _strip_default_arg(param: str) -> str:
    angle_depth = 0
    paren_depth = 0
    for i, ch in enumerate(param):
        if ch == "<":
            angle_depth += 1
        elif ch == ">" and angle_depth > 0:
            angle_depth -= 1
        elif ch == "(":
            paren_depth += 1
        elif ch == ")" and paren_depth > 0:
            paren_depth -= 1
        elif ch == "=" and angle_depth == 0 and paren_depth == 0:
            return param[:i]
    return param


def _normalize_type(text: str) -> str:
    text = re.sub(r"\s+", " ", text).strip()
    text = re.sub(r"\s*([<>,()&*])\s*", r"\1", text)
    text = re.sub(r"\s*=\s*", "=", text)
    return text


def _normalize_param(param: str) -> str:
    param = _strip_default_arg(param)
    param = re.sub(r"\s+", " ", param).strip()
    # Parameter names are not part of the ABI signature; remove the common
    # named-parameter shape while preserving unnamed type-only declarations.
    name_match = re.match(r"(?P<type>.+\S)\s+[A-Za-z_]\w*$", param)
    if name_match:
        param = name_match.group("type")
    return _normalize_type(param)


def _canonical_virtual_signature(raw_decl: str) -> tuple[str, str]:
    decl = re.sub(r"^\s*virtual\b", "", raw_decl, count=1).strip()
    open_paren = decl.find("(")
    if open_paren < 0:
        raise ValueError("virtual method declaration missing parameter list")
    close_paren = _matching_paren(decl, open_paren)

    before_paren = decl[:open_paren].strip()
    params = decl[open_paren + 1:close_paren]
    after_paren = decl[close_paren + 1:].strip()

    name_match = re.search(r"([~A-Za-z_]\w*)\s*$", before_paren)
    if not name_match:
        raise ValueError("virtual method declaration missing method name")
    name = name_match.group(1)

    canonical_params = ",".join(_normalize_param(p) for p in _split_params(params))
    signature = (
        f"{_normalize_type(before_paren)}"
        f"({canonical_params})"
        f"{_normalize_type(after_paren)}"
    )
    return name, signature


def virtual_order(text: str, class_name: str) -> list[VirtualDecl]:
    body = class_body(strip_comments(text), class_name)
    decls: list[VirtualDecl] = []
    for match in re.finditer(r"\bvirtual\b(?P<decl>.*?)(?:;|{)", body, re.DOTALL):
        raw_decl = match.group("decl").strip()
        try:
            name, signature = _canonical_virtual_signature(f"virtual {raw_decl}")
        except ValueError:
            continue
        decls.append(VirtualDecl(name=name, signature=signature))
    return decls


def render_mismatch(surface: str, old: list[VirtualDecl], new: list[VirtualDecl]) -> str:
    lines = [f"{surface}: virtual order is not additive-only"]
    common = min(len(old), len(new))
    first_bad = next(
        (i for i in range(common) if old[i].signature != new[i].signature),
        common,
    )
    if first_bad < len(old):
        lines.append(f"  first mismatch at index {first_bad}:")
        lines.append(f"    base:    {old[first_bad].signature}")
        lines.append(
            "    current: "
            f"{new[first_bad].signature if first_bad < len(new) else '<missing>'}"
        )
    if len(new) < len(old):
        lines.append("  current order removed virtual method(s)")
    lines.append(
        "  allowed change: append new virtual methods after all existing ones; "
        "do not re-signature existing virtuals"
    )
    return "\n".join(lines)


def check_surface(root: Path, base: str, class_name: str, rel_path: str) -> str | None:
    old_text = git_show(base, rel_path)
    if old_text is None:
        return f"{class_name}: could not read {rel_path} at {base}"

    current_path = root / rel_path
    if not current_path.exists():
        return f"{class_name}: current file missing: {rel_path}"

    try:
        old = virtual_order(old_text, class_name)
        new = virtual_order(current_path.read_text(), class_name)
    except ValueError as exc:
        return f"{class_name}: {exc}"

    if [v.signature for v in new[:len(old)]] != [v.signature for v in old]:
        return render_mismatch(class_name, old, new)
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--mode", choices=("hint", "report"), default="report")
    args = parser.parse_args(argv)

    root = repo_root()
    if root is None:
        print("node_abi_gate: not in a git working tree", file=sys.stderr)
        return 0 if args.mode == "hint" else 2

    findings = [
        finding
        for class_name, rel_path in SURFACES
        if (finding := check_surface(root, args.base, class_name, rel_path))
    ]

    if not findings:
        print("node_abi_gate: Processor and PluginSlot virtual order is additive-only")
        return 0

    print("\n\n".join(findings), file=sys.stderr)
    return 0 if args.mode == "hint" else 1


if __name__ == "__main__":
    raise SystemExit(main())
