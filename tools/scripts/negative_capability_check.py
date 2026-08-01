#!/usr/bin/env python3
"""Require every authorable playback-compile refusal to carry a written reason.

A *negative capability* is a construct a user can author — it has a model type,
a schema field, a command, a migration — that the playback compiler then
refuses. Authoring one is worse than the feature not existing: the document
becomes unplayable, and nothing at authoring time says so. This check makes
adding one cost a written reason and an owner rather than nothing.

The check reads the refusal-shaped members of `CompileErrorCode`, finds every
site that raises one, decides whether the refused construct is reachable from
the timeline authoring surface, and requires an allowlist entry for each
authorable refusal. It does not forbid negative capabilities; it forbids
undocumented ones, and it fails on an allowlist entry whose raise site is gone
so the written reasons cannot outlive the code.

A refusal counts as authorable when the source leading to the raise reads a
symbol declared in the timeline module's public headers or named by the
timeline schema — a model accessor, a model type, an enum constant, a schema
field. Reaching a construct through those is what "a user can author it" means.

What this check cannot see:

  * A refusal expressed some other way. Returning an empty result, dropping the
    construct silently, clamping it, or substituting a default are all the same
    defect and none of them name a `CompileErrorCode`. Only a named refusal is
    in range.
  * Refusals raised outside `CompileErrorCode`. Importers, exporters, and
    renderers carry their own error enums; a construct refused there is out of
    range even when it is equally authorable.
  * Authorability at a distance. The read that proves a construct authorable
    must appear within `AUTHORING_LOOKBACK_LINES` source lines above the raise.
    A guard whose authored input arrives through a value computed further away,
    or through a field of an internal struct that no longer names its model
    origin, reads as not authorable.
  * Whether a refusal is correct. An allowlist entry records that someone wrote
    down why; it does not record that the reason is good.

Passing this check therefore means every refusal it can see is accounted for,
not that the compiler accepts everything a user can author.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import tempfile
from pathlib import Path


COMPILE_ERROR_HEADER = Path("core/playback/include/pulp/playback/program_compiler.hpp")
ALLOWLIST_PATH = Path("tools/scripts/negative_capability_allowlist.json")
AUTHORING_HEADER_DIR = Path("core/timeline/include/pulp/timeline")
AUTHORING_SCHEMA = Path("core/timeline/schema/timeline_schema.json")

# Directories searched for raise sites. Tests are excluded on purpose: a test
# that provokes a refusal is asserting the refusal exists, not adding one.
SOURCE_ROOTS = (Path("core"), Path("tools"), Path("examples"), Path("apple"), Path("inspect"))
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".mm"}

# Members whose name carries any of these words refuse a construct rather than
# report an operational failure. Matched anywhere in the name so a future
# `UnsupportedFoo` is caught alongside `FooUnsupported`.
REFUSAL_WORDS = ("Unsupported", "NotSupported", "Rejected", "Refused", "Disallowed")

# How far above a raise the authored read may sit. Wide enough to cover a guard
# whose condition is assembled over several statements, narrow enough that an
# unrelated neighbour does not lend its symbols.
AUTHORING_LOOKBACK_LINES = 40

MINIMUM_REASON_LENGTH = 24

ENUM_RE = re.compile(r"enum\s+class\s+CompileErrorCode\s*(?::[^{]*)?\{(.*?)\}", re.DOTALL)
RAISE_RE = re.compile(r"\bCompileErrorCode::([A-Za-z_]\w*)")
# A field whose declared default happens to name a code declares nothing about
# where that code is raised.
FIELD_DEFAULT_RE = re.compile(r"^\s*CompileErrorCode\s+\w+\s*=\s*CompileErrorCode::")

BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")
# Blanked before comments are found: a `//` inside a string would otherwise end
# the line early and take a raise sitting after it out of range.
STRING_LITERAL_RE = re.compile(r'"(?:\\.|[^"\\\n])*"' r"|'(?:\\.|[^'\\\n])*'")
STD_QUALIFIED_RE = re.compile(r"\bstd::[A-Za-z_]\w*")
MEMBER_ACCESS_RE = re.compile(r"(?:\.|->|::)\s*([A-Za-z_]\w*)")

TYPE_DECL_RE = re.compile(r"\b(?:class|struct|enum\s+class|enum)\s+([A-Za-z_]\w*)")
CONSTANT_DECL_RE = re.compile(r"\bconstexpr\s+[\w:<>,\s\*&]+?\b(k[A-Za-z_]\w*)\s*(?:=|\{)")
FUNCTION_DECL_RE = re.compile(
    r"^\s*(?:\[\[[^\]]*\]\]\s*)*"
    r"(?:template\s*<[^>]*>\s*)?"
    r"(?:(?:const|constexpr|static|inline|virtual|explicit|friend|noexcept)\s+)*"
    r"[\w:][\w:<>,\s\*&]*?\b([a-z_]\w*)\s*\("
)
FIELD_DECL_RE = re.compile(
    r"^\s*(?:(?:const|constexpr|static|inline|mutable)\s+)*"
    r"[A-Za-z_][\w:<>,\s\*&]*?\b([a-z_]\w*)\s*(?:=[^;]*)?;\s*$"
)
ENUM_BLOCK_RE = re.compile(r"\benum\s+class\s+[A-Za-z_]\w*\s*(?::[^{]*)?\{(.*?)\}", re.DOTALL)

# Names too common to distinguish a model read from any other code. Admitting
# them would classify every refusal as authorable and the check would stop
# saying anything.
GENERIC_NAMES = frozenset(
    {
        "at", "back", "begin", "clear", "code", "contains", "count", "data",
        "empty", "end", "error", "find", "first", "front", "get", "get_if",
        "has_value", "holds_alternative", "id", "index", "insert", "kind",
        "max", "min", "move", "name", "push_back", "emplace_back", "reserve",
        "resize", "second", "size", "type", "valid", "value", "visit",
    }
)


def strip_comments(text: str) -> str:
    """Blank out string and comment text, preserving line count.

    Comments in this repo describe the model they guard, so leaving them in
    would let a comment supply the authored symbol its code does not read.
    """
    def blank(match: re.Match[str]) -> str:
        return "".join("\n" if character == "\n" else " " for character in match.group(0))

    return LINE_COMMENT_RE.sub(blank, BLOCK_COMMENT_RE.sub(blank, STRING_LITERAL_RE.sub(blank, text)))


def source_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for source_root in SOURCE_ROOTS:
        directory = root / source_root
        if not directory.is_dir():
            continue
        for path in sorted(directory.rglob("*")):
            if path.suffix in SOURCE_SUFFIXES and path.is_file():
                files.append(path)
    return files


def refusal_codes(root: Path) -> tuple[list[str], list[str]]:
    """Return (all enum members, members whose name refuses a construct)."""
    header = root / COMPILE_ERROR_HEADER
    if not header.is_file():
        return ([], [])
    body = ENUM_RE.search(strip_comments(header.read_text(encoding="utf-8")))
    if not body:
        return ([], [])
    members = [
        token.strip().split("=")[0].strip()
        for token in body.group(1).split(",")
        if token.strip()
    ]
    members = [member for member in members if re.fullmatch(r"[A-Za-z_]\w*", member)]
    refusals = [
        member for member in members if any(word in member for word in REFUSAL_WORDS)
    ]
    return (members, refusals)


def authoring_symbols(root: Path) -> set[str]:
    """Symbols a document author can reach: model declarations and schema fields."""
    symbols: set[str] = set()
    header_dir = root / AUTHORING_HEADER_DIR
    if header_dir.is_dir():
        for header in sorted(header_dir.rglob("*.hpp")):
            text = strip_comments(header.read_text(encoding="utf-8"))
            symbols.update(TYPE_DECL_RE.findall(text))
            symbols.update(CONSTANT_DECL_RE.findall(text))
            for block in ENUM_BLOCK_RE.findall(text):
                for token in block.split(","):
                    candidate = token.strip().split("=")[0].strip()
                    if re.fullmatch(r"[A-Za-z_]\w*", candidate):
                        symbols.add(candidate)
            for line in text.splitlines():
                function = FUNCTION_DECL_RE.match(line)
                if function:
                    symbols.add(function.group(1))
                field = FIELD_DECL_RE.match(line)
                if field:
                    symbols.add(field.group(1))
    schema = root / AUTHORING_SCHEMA
    if schema.is_file():
        def walk(node: object) -> None:
            if isinstance(node, dict):
                for key, child in node.items():
                    if re.fullmatch(r"[A-Za-z_]\w*", key):
                        symbols.add(key)
                    walk(child)
            elif isinstance(node, list):
                for child in node:
                    walk(child)

        walk(json.loads(schema.read_text(encoding="utf-8")))
    return {symbol for symbol in symbols if symbol not in GENERIC_NAMES}


def authored_reads(lines: list[str], raise_index: int, symbols: set[str]) -> list[str]:
    """Authoring-surface symbols read in the window leading to a raise.

    Only member accesses and scope-qualified names count. A bare local carries
    no evidence of where its value came from, and `std::` names are dropped so
    a container operation cannot pass for a model read.
    """
    start = max(0, raise_index - AUTHORING_LOOKBACK_LINES)
    window = STD_QUALIFIED_RE.sub(" ", "\n".join(lines[start : raise_index + 1]))
    found = {
        token
        for token in MEMBER_ACCESS_RE.findall(window)
        if token not in GENERIC_NAMES and token in symbols
    }
    return sorted(found)


def raise_sites(root: Path, refusals: set[str], symbols: set[str]) -> list[dict[str, object]]:
    sites: list[dict[str, object]] = []
    for path in source_files(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        if "CompileErrorCode::" not in text:
            continue
        lines = strip_comments(text).splitlines()
        for index, line in enumerate(lines):
            if FIELD_DEFAULT_RE.match(line):
                continue
            for code in RAISE_RE.findall(line):
                if code not in refusals:
                    continue
                sites.append(
                    {
                        "code": code,
                        "file": path.relative_to(root).as_posix(),
                        "line": index + 1,
                        "reads": authored_reads(lines, index, symbols),
                    }
                )
    return sites


def load_allowlist(root: Path) -> tuple[list[dict[str, object]], list[str]]:
    path = root / ALLOWLIST_PATH
    if not path.is_file():
        return ([], [f"missing allowlist {ALLOWLIST_PATH.as_posix()}"])
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as failure:
        return ([], [f"unparsable allowlist {ALLOWLIST_PATH.as_posix()}: {failure}"])
    entries = document.get("refusals")
    if not isinstance(entries, list):
        return ([], [f"allowlist {ALLOWLIST_PATH.as_posix()} has no refusals array"])

    errors: list[str] = []
    valid: list[dict[str, object]] = []
    for entry in entries:
        if not isinstance(entry, dict):
            errors.append(f"allowlist entry is not an object: {entry!r}")
            continue
        code = entry.get("code")
        file = entry.get("file")
        owner = entry.get("owner")
        reason = entry.get("reason")
        status = entry.get("status")
        label = f"{code}@{file}"
        if not isinstance(code, str) or not code:
            errors.append(f"allowlist entry {entry!r} has no code")
            continue
        if not isinstance(file, str) or not file:
            errors.append(f"allowlist entry {code} has no file")
            continue
        if not isinstance(owner, str) or not owner.strip():
            errors.append(f"allowlist entry {label} has no owner")
        if not isinstance(reason, str) or len(reason.strip()) < MINIMUM_REASON_LENGTH:
            errors.append(
                f"allowlist entry {label} needs a reason of at least "
                f"{MINIMUM_REASON_LENGTH} characters"
            )
        if status not in {"live-defect", "intended"}:
            errors.append(
                f"allowlist entry {label} needs status live-defect or intended, got {status!r}"
            )
        valid.append(entry)
    return (valid, errors)


def verify(root: Path) -> list[str]:
    errors: list[str] = []
    members, refusals = refusal_codes(root)
    if not members:
        return [f"could not read CompileErrorCode from {COMPILE_ERROR_HEADER.as_posix()}"]

    symbols = authoring_symbols(root)
    if not symbols:
        return [f"could not read the authoring surface from {AUTHORING_HEADER_DIR.as_posix()}"]

    entries, allowlist_errors = load_allowlist(root)
    errors.extend(allowlist_errors)

    sites = raise_sites(root, set(refusals), symbols)
    allowed = {
        (entry.get("code"), entry.get("file"))
        for entry in entries
        if isinstance(entry.get("code"), str) and isinstance(entry.get("file"), str)
    }

    covered: set[tuple[str, str]] = set()
    for site in sites:
        key = (site["code"], site["file"])
        if not site["reads"]:
            continue
        if key in allowed:
            covered.add(key)
            continue
        errors.append(
            f"{site['file']}:{site['line']} raises CompileErrorCode::{site['code']} "
            f"for an authorable construct (reads {', '.join(site['reads'][:6])}) "
            f"with no entry in {ALLOWLIST_PATH.as_posix()}"
        )

    for key in sorted(allowed - covered):
        errors.append(
            f"allowlist entry {key[0]}@{key[1]} has no authorable raise site; "
            "remove it so the written reasons cannot outlive the code"
        )
    return errors


def describe(root: Path) -> int:
    members, refusals = refusal_codes(root)
    symbols = authoring_symbols(root)
    print(f"CompileErrorCode members: {len(members)}")
    print(f"refusal-shaped members: {', '.join(refusals) or '(none)'}")
    print(f"authoring-surface symbols: {len(symbols)}")
    for site in raise_sites(root, set(refusals), symbols):
        verdict = "authorable" if site["reads"] else "not-authorable"
        reads = ", ".join(site["reads"][:8]) or "-"
        print(f"{site['file']}:{site['line']} {site['code']} {verdict} [{reads}]")
    return 0


def run_selftest() -> int:
    """Prove the check goes red on each way a negative capability arrives.

    A gate that cannot fail is indistinguishable from no gate, so every rule
    below is exercised against a fixture that breaks it and then restored.
    """
    repo = Path(__file__).resolve().parents[2]
    if verify(repo):
        print("selftest rejected the repository as it stands")
        return 1

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory) / "repo"
        for path in (
            COMPILE_ERROR_HEADER,
            ALLOWLIST_PATH,
            AUTHORING_SCHEMA,
            Path("core/playback/src/program_compiler.cpp"),
            Path("core/playback/src/sequence_content_lowerer.cpp"),
        ):
            (root / path).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(repo / path, root / path)
        shutil.copytree(repo / AUTHORING_HEADER_DIR, root / AUTHORING_HEADER_DIR)

        if verify(root):
            print("selftest rejected the copied fixture")
            return 1

        allowlist = root / ALLOWLIST_PATH
        original_allowlist = allowlist.read_text(encoding="utf-8")
        document = json.loads(original_allowlist)

        # Dropping a known refusal must name it again.
        for dropped in ("MidiExpressionLaneUnsupported", "TrimmedMidiLaneUnsupported"):
            trimmed = {
                "refusals": [
                    entry for entry in document["refusals"] if entry["code"] != dropped
                ]
            }
            allowlist.write_text(json.dumps(trimmed, indent=2) + "\n", encoding="utf-8")
            if not any(dropped in error and "no entry in" in error for error in verify(root)):
                print(f"selftest missed the dropped {dropped} entry")
                return 1

        # An entry whose reason went missing must not pass for a written reason.
        for field, blanked in (("reason", "too short"), ("owner", "  "), ("status", "maybe")):
            weakened = json.loads(original_allowlist)
            weakened["refusals"][0][field] = blanked
            allowlist.write_text(json.dumps(weakened, indent=2) + "\n", encoding="utf-8")
            if not any(field in error for error in verify(root)):
                print(f"selftest missed a blanked {field}")
                return 1

        # An entry left behind after its raise site is gone must be removed.
        stale = json.loads(original_allowlist)
        stale["refusals"].append(
            {
                "code": stale["refusals"][0]["code"],
                "file": "core/playback/src/nothing_raises_here.cpp",
                "owner": "playback",
                "status": "intended",
                "reason": "a reason long enough to satisfy the minimum length rule",
            }
        )
        allowlist.write_text(json.dumps(stale, indent=2) + "\n", encoding="utf-8")
        if not any("has no authorable raise site" in error for error in verify(root)):
            print("selftest missed a stale allowlist entry")
            return 1
        allowlist.write_text(original_allowlist, encoding="utf-8")
        if verify(root):
            print("selftest rejected the restored allowlist")
            return 1

        synthetic = root / "core/playback/src/selftest_refusal.cpp"

        # A new raise guarded on an authored read is the defect this exists for.
        synthetic.write_text(
            "#include <pulp/playback/program_compiler.hpp>\n"
            "namespace pulp::playback {\n"
            "CompileError refuse(const timeline::Clip& clip) {\n"
            "    if (!clip.playback_properties().fade_in_duration)\n"
            "        return {CompileErrorCode::MidiExpressionLaneUnsupported, clip.id(), 0};\n"
            "    return {};\n"
            "}\n"
            "}\n",
            encoding="utf-8",
        )
        if not any(
            "selftest_refusal.cpp" in error and "authorable" in error for error in verify(root)
        ):
            print("selftest missed a synthetic authorable refusal")
            return 1

        # A raise that never reads the document refuses nothing a user authored.
        synthetic.write_text(
            "#include <pulp/playback/program_compiler.hpp>\n"
            "namespace pulp::playback {\n"
            "CompileError refuse(unsigned long budget, unsigned long charged) {\n"
            "    if (charged > budget)\n"
            "        return {CompileErrorCode::MidiExpressionLaneUnsupported, {}, 0};\n"
            "    return {};\n"
            "}\n"
            "}\n",
            encoding="utf-8",
        )
        if verify(root):
            print("selftest rejected a refusal that reads nothing authorable")
            return 1
        synthetic.unlink()

        # A refusal-shaped member added to the enum must be picked up by name.
        header = root / COMPILE_ERROR_HEADER
        original_header = header.read_text(encoding="utf-8")
        header.write_text(
            original_header.replace(
                "    MidiExpressionLaneUnsupported,\n",
                "    MidiExpressionLaneUnsupported,\n    ClipFadeRejected,\n",
            ),
            encoding="utf-8",
        )
        if "ClipFadeRejected" not in refusal_codes(root)[1]:
            print("selftest missed a differently spelled refusal member")
            return 1
        synthetic.write_text(
            "#include <pulp/playback/program_compiler.hpp>\n"
            "namespace pulp::playback {\n"
            "CompileError refuse(const timeline::Clip& clip) {\n"
            "    if (clip.time_anchor() != timeline::ClipTimeAnchor::Musical)\n"
            "        return {CompileErrorCode::ClipFadeRejected, clip.id(), 0};\n"
            "    return {};\n"
            "}\n"
            "}\n",
            encoding="utf-8",
        )
        if not any("ClipFadeRejected" in error for error in verify(root)):
            print("selftest missed a raise of a differently spelled refusal member")
            return 1
        synthetic.unlink()
        header.write_text(original_header, encoding="utf-8")
        if verify(root):
            print("selftest rejected the restored fixture")
            return 1

    print("negative_capability_selftest=true")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--describe", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return run_selftest()
    root = args.repo_root.resolve()
    if args.describe:
        return describe(root)

    errors = verify(root)
    if errors:
        for error in errors:
            print(error)
        return 1
    print("negative_capability_verified=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
