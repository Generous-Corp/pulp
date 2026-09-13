#!/usr/bin/env python3
import argparse
import ast
import re
import sys
from pathlib import Path


def read_text(path: Path):
    return path.read_text(encoding="utf-8")


def require(condition, message, errors):
    if not condition:
        errors.append(message)


def resolve_struct_name(header_text, name, _seen=None):
    """Follow `using <name> = <target>;` aliases to the struct that defines the body.

    The probe result types are declared once and re-exported under per-entry-point
    aliases, so the contract's subject is often an alias rather than a struct.
    """
    seen = set() if _seen is None else _seen
    if name in seen:
        raise ValueError(f"cyclic alias chain for {name}")
    seen.add(name)
    if re.search(r"struct\s+" + re.escape(name) + r"\s*\{", header_text):
        return name
    alias = re.search(
        r"using\s+" + re.escape(name) + r"\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;",
        header_text,
    )
    if alias:
        return resolve_struct_name(header_text, alias.group(1), seen)
    return None


def extract_result_bool_fields(header_text):
    resolved = resolve_struct_name(header_text, "HardcodedCubeRenderResult")
    if resolved is None:
        raise ValueError("missing HardcodedCubeRenderResult")
    match = re.search(
        r"struct\s+" + re.escape(resolved) + r"\s*\{(?P<body>.*?)\n\};",
        header_text,
        re.DOTALL,
    )
    if not match:
        raise ValueError("missing HardcodedCubeRenderResult")
    return set(re.findall(r"\bbool\s+([A-Za-z_][A-Za-z0-9_]*)\s*=", match.group("body")))


def extract_probe_fields(verifier_text):
    tree = ast.parse(verifier_text)
    for node in tree.body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id == "PROBE_FIELDS":
                    value = ast.literal_eval(node.value)
                    if not isinstance(value, set):
                        raise ValueError("PROBE_FIELDS must be a set literal")
                    return set(value)
    raise ValueError("missing PROBE_FIELDS")


def extract_printed_result_bool_fields(probe_source):
    printed = set()
    for key, member in re.findall(
            r'print_bool\("([A-Za-z_][A-Za-z0-9_]*)",\s*result\.([A-Za-z_][A-Za-z0-9_]*)\)',
            probe_source,
            re.DOTALL):
        if key != member:
            raise ValueError(
                f"probe print_bool key/member mismatch: {key} vs {member}")
        printed.add(member)
    for key, member in re.findall(
            r'"([A-Za-z_][A-Za-z0-9_]*)="\s*<<\s*\(result\.([A-Za-z_][A-Za-z0-9_]*)\s*\?',
            probe_source):
        if key != member:
            raise ValueError(
                f"probe bool cout key/member mismatch: {key} vs {member}")
        printed.add(member)
    return printed


def main():
    parser = argparse.ArgumentParser(
        description="Verify renderer probe output covers every Renderer3D result boolean.")
    parser.add_argument("--renderer-header", type=Path, required=True)
    parser.add_argument("--probe-source", type=Path, required=True)
    parser.add_argument("--probe-verifier", type=Path, required=True)
    args = parser.parse_args()

    for label, path in (
            ("renderer_header", args.renderer_header),
            ("probe_source", args.probe_source),
            ("probe_verifier", args.probe_verifier)):
        if not path.exists():
            print(f"{label}_exists=false path={path}")
            return 2

    header_bools = extract_result_bool_fields(read_text(args.renderer_header))
    printed_bools = extract_printed_result_bool_fields(read_text(args.probe_source))
    verifier_fields = extract_probe_fields(read_text(args.probe_verifier))

    errors = []
    require(
        header_bools == printed_bools,
        "probe printed bool fields drift: missing "
        f"{sorted(header_bools - printed_bools)!r}, extra "
        f"{sorted(printed_bools - header_bools)!r}",
        errors,
    )
    require(
        header_bools.issubset(verifier_fields),
        "verifier PROBE_FIELDS missing result bools: "
        f"{sorted(header_bools - verifier_fields)!r}",
        errors,
    )

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print(f"renderer_probe_public_surface_verified={len(header_bools)} bools")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        raise SystemExit(1)
