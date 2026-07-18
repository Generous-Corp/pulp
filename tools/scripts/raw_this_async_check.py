#!/usr/bin/env python3
"""Reject unreviewed raw-owner captures in long-lived worker threads."""

from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
ALLOWLIST = Path(__file__).with_name("raw_this_async_allowlist.json")
SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".hpp", ".h", ".mm", ".m"}

LAMBDA_THREAD = re.compile(
    r"std::(?:j?thread)(?:\s+[A-Za-z_]\w*)?\s*\(\s*\[(?P<capture>[^\]]*)\]",
    re.MULTILINE | re.DOTALL,
)
MEMBER_THREAD = re.compile(
    r"std::(?:j?thread)\s*\(\s*&[A-Za-z_][\w:]*\s*,\s*this\b",
    re.MULTILINE,
)


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    kind: str
    snippet: str


def normalize(text: str) -> str:
    return " ".join(text.split())


def capture_is_raw(capture: str) -> bool:
    parts = [part.strip() for part in normalize(capture).split(",") if part.strip()]
    return any(
        part == "this"
        or part == "&"
        or part.startswith("&")
        or ".get()" in part
        or "->get()" in part
        for part in parts
    )


def scan_text(text: str, rel_path: str) -> list[Finding]:
    findings: list[Finding] = []
    for match in LAMBDA_THREAD.finditer(text):
        if capture_is_raw(match.group("capture")):
            findings.append(Finding(
                rel_path,
                text.count("\n", 0, match.start()) + 1,
                "lambda",
                normalize(match.group(0)),
            ))
    for match in MEMBER_THREAD.finditer(text):
        findings.append(Finding(
            rel_path,
            text.count("\n", 0, match.start()) + 1,
            "member",
            normalize(match.group(0)),
        ))
    return findings


def source_files(root: Path) -> list[Path]:
    return sorted(
        path for path in (root / "core").rglob("*")
        if path.is_file() and path.suffix in SOURCE_SUFFIXES
    )


def load_allowlist(path: Path) -> list[dict[str, str]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    entries = data.get("entries")
    if not isinstance(entries, list):
        raise ValueError("allowlist must contain an entries array")
    for entry in entries:
        if not all(isinstance(entry.get(key), str) and entry[key]
                   for key in ("path", "needle", "reason")):
            raise ValueError("every allowlist entry needs path, needle, and reason")
    return entries


def is_allowed(finding: Finding, entries: list[dict[str, str]]) -> bool:
    return any(
        entry["path"] == finding.path and entry["needle"] in finding.snippet
        for entry in entries
    )


def run(root: Path, allowlist: Path) -> int:
    entries = load_allowlist(allowlist)
    failures: list[Finding] = []
    for path in source_files(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = path.relative_to(root).as_posix()
        failures.extend(
            finding for finding in scan_text(text, rel)
            if not is_allowed(finding, entries)
        )

    if not failures:
        print("raw-this async check: PASS")
        return 0

    print("raw-this async check: FAIL", file=sys.stderr)
    for finding in failures:
        print(
            f"  {finding.path}:{finding.line}: unreviewed {finding.kind} worker capture: "
            f"{finding.snippet}",
            file=sys.stderr,
        )
    print(
        "Capture shared/owned state instead, or add a stable needle and teardown "
        "review reason to tools/scripts/raw_this_async_allowlist.json.",
        file=sys.stderr,
    )
    return 1


def selftest() -> int:
    hazardous = """
      auto a = std::thread([this] { run(); });
      auto b = std::thread([&, token] { owner.tick(); });
      auto c = std::thread([raw = owner.get()] { raw->tick(); });
      auto d = std::thread(&Worker::run, this, 7);
      std::thread e([this, &done] { run(); });
    """
    safe = """
      auto a = std::thread([state = shared_state] { state->run(); });
      auto b = std::thread([token] { if (token->load()) tick(); });
    """
    if len(scan_text(hazardous, "core/fake.cpp")) != 5:
        print("raw-this async check selftest missed a hazardous form", file=sys.stderr)
        return 1
    if scan_text(safe, "core/safe.cpp"):
        print("raw-this async check selftest flagged safe captures", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        source = root / "core" / "fake.cpp"
        source.parent.mkdir(parents=True)
        source.write_text("auto t = std::thread([this] { run(); });\n", encoding="utf-8")
        allow = root / "allow.json"
        allow.write_text('{"entries": []}', encoding="utf-8")
        if run(root, allow) == 0:
            print("raw-this async check selftest did not prove failure", file=sys.stderr)
            return 1

    print("raw-this async check selftest: PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=REPO)
    parser.add_argument("--allowlist", type=Path, default=ALLOWLIST)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    return selftest() if args.selftest else run(args.root.resolve(), args.allowlist)


if __name__ == "__main__":
    raise SystemExit(main())
