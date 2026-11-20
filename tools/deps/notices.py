#!/usr/bin/env python3
"""Generate Pulp's per-SDK third-party notices manifest.

Reads tools/deps/manifest.json and NOTICE.md and writes

    <out>/notices.json          schema pulp.notices.v1
    <out>/licenses/<slug>.txt   one notice text per entry

for every manifest entry that names ``link_targets``: the CMake targets whose
presence in a plug-in's link closure means that dependency's code ships in the
plug-in. ``pulp_add_plugin`` unions this manifest (and any other SDK's) over each
plug-in's closure into the bundle's ThirdPartyNotices files; see
tools/cmake/PulpThirdPartyNotices.cmake.

Files are rewritten only when their content changes, so a reconfigure does not
relink every plug-in. An entry with ``link_targets`` but no NOTICE.md section is
an error: a linked dependency with no notice text is the gap this exists to close.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = "pulp.notices.v1"
SPDX_EXPRESSION = re.compile(
    r"^[A-Za-z0-9.+-]+(?: (?:OR|AND|WITH) [A-Za-z0-9.+-]+)*$")


def notice_sections(notice_text: str) -> dict[str, str]:
    """Return {heading: body} for every ``## `` section of a NOTICE file."""
    sections: dict[str, str] = {}
    for block in re.split(r"^## ", notice_text, flags=re.M)[1:]:
        name, _, body = block.partition("\n")
        body = re.sub(r"\n-{3,}\s*$", "\n", body.rstrip() + "\n")
        sections[name.strip()] = body.strip() + "\n"
    return sections


def copyright_lines(body: str) -> str:
    """The first run of copyright lines in a notice body, joined by newlines."""
    lines: list[str] = []
    for line in body.splitlines():
        stripped = line.strip()
        if re.match(r"^(Copyright\b|\([cC]\)|©)", stripped):
            lines.append(stripped)
        elif lines and stripped:
            if stripped.lower().startswith("all rights reserved"):
                lines.append(stripped)
                continue
            break
    return "\n".join(lines)


def slug(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def spdx_id(dep: dict) -> str:
    if dep.get("spdx"):
        return dep["spdx"]
    license_name = dep.get("license", "")
    if SPDX_EXPRESSION.match(license_name):
        return license_name
    return "LicenseRef-" + slug(license_name or dep["name"])


def source_url(dep: dict) -> str:
    url = dep.get("repository", "")
    return url[:-4] if url.endswith(".git") else url


def build_manifest(deps: list[dict], sections: dict[str, str],
                   sdk_version: str) -> tuple[dict, dict[str, str], list[str]]:
    """Return (manifest, {relative text path: text}, errors)."""
    entries: list[dict] = []
    texts: dict[str, str] = {}
    errors: list[str] = []
    for dep in deps:
        targets = dep.get("link_targets")
        if not targets:
            continue
        name = dep["name"]
        body = sections.get(dep.get("notice_section", name))
        if body is None:
            errors.append(f"{name}: has link_targets but no '## {name}' section in NOTICE.md")
            continue
        rel = f"licenses/{slug(name)}.txt"
        texts[rel] = body
        entries.append({
            "name": name,
            "version": dep.get("version", ""),
            "license": spdx_id(dep),
            "license_name": dep.get("license", ""),
            "copyright": copyright_lines(body),
            "source": source_url(dep),
            "license_text": rel,
            "sha256": hashlib.sha256(body.encode()).hexdigest(),
            "required_by": list(targets),
            "feature": dep.get("link_feature", ""),
        })
    entries.sort(key=lambda e: e["name"].lower())
    manifest = {
        "schema": SCHEMA,
        "sdk": "Pulp",
        "sdk_version": sdk_version,
        "namespaces": ["Pulp", "pulp"],
        "entries": entries,
    }
    return manifest, texts, errors


def write_if_changed(path: Path, content: str) -> bool:
    if path.exists() and path.read_text() == content:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    return True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", required=True, type=Path,
                        help="directory that receives notices.json and licenses/")
    parser.add_argument("--manifest", type=Path,
                        default=ROOT / "tools" / "deps" / "manifest.json")
    parser.add_argument("--notice", type=Path, default=ROOT / "NOTICE.md")
    parser.add_argument("--sdk-version", default="")
    args = parser.parse_args(argv)

    deps = json.loads(args.manifest.read_text())["dependencies"]
    sections = notice_sections(args.notice.read_text())
    manifest, texts, errors = build_manifest(deps, sections, args.sdk_version)
    if errors:
        for error in errors:
            print(f"notices: {error}", file=sys.stderr)
        return 1
    for rel, text in texts.items():
        write_if_changed(args.out / rel, text)
    write_if_changed(args.out / "notices.json", json.dumps(manifest, indent=2) + "\n")
    print(f"notices: {len(manifest['entries'])} entries -> {args.out / 'notices.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
