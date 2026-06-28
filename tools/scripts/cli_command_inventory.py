#!/usr/bin/env python3
"""Shared CLI command inventory helpers for Pulp sync gates."""

from __future__ import annotations

import re
from pathlib import Path


def camel_to_kebab(name: str) -> str:
    """Convert a Rust enum variant name into the default clap command spelling."""
    return re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "-", name).lower()


def extract_rust_commands_from_main(rust_main: Path) -> set[str]:
    """Return user-visible Rust front-end command names from ``enum Command``."""
    if not rust_main.exists():
        return set()

    names: set[str] = set()
    pending_name: str | None = None
    in_command_enum = False
    depth = 0

    for line in rust_main.read_text().splitlines():
        if not in_command_enum:
            if re.search(r"\benum\s+Command\s*\{", line):
                in_command_enum = True
                depth = line.count("{") - line.count("}")
            continue

        if depth <= 0:
            break

        attr = re.search(r'#\s*\[\s*command\s*\([^]]*\bname\s*=\s*"([^"]+)"', line)
        if attr:
            pending_name = attr.group(1)
        else:
            variant = re.match(r"\s*([A-Z][A-Za-z0-9_]*)\s*(?:\(|,|\{)", line)
            if variant:
                names.add(pending_name or camel_to_kebab(variant.group(1)))
                pending_name = None

        depth += line.count("{") - line.count("}")

    return names


def extract_rust_command_names(root: Path) -> set[str]:
    """Parse Rust-native top-level command names from experimental/pulp-rs."""
    return extract_rust_commands_from_main(
        root / "experimental" / "pulp-rs" / "src" / "main.rs"
    )
