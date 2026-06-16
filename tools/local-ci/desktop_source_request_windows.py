"""Windows desktop source prepare-command helpers."""

from __future__ import annotations

import re


def split_windows_prepare_commands(command: str) -> list[str]:
    parts: list[str] = []
    current: list[str] = []
    quote: str | None = None
    for ch in command:
        if quote is not None:
            current.append(ch)
            if ch == quote:
                quote = None
            continue
        if ch in {"'", '"'}:
            quote = ch
            current.append(ch)
            continue
        if ch in {";", "\n"}:
            segment = "".join(current).strip()
            if segment:
                parts.append(segment)
            current = []
            continue
        current.append(ch)
    segment = "".join(current).strip()
    if segment:
        parts.append(segment)
    return parts


def validate_windows_prepare_commands(commands: list[str]) -> None:
    suspicious = [cmd for cmd in commands if re.search(r"(^|[\s=])'[^']+'(?=$|[\s&|;])", cmd)]
    if suspicious:
        sample = suspicious[0]
        raise ValueError(
            "Windows prepare commands run under cmd.exe, where single-quoted tokens are literal text. "
            "Use double quotes for paths, generator names, and arguments instead. "
            f"Suspicious command: {sample}"
        )
