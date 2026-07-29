#!/usr/bin/env python3
"""Self-test for the interchange capability emitter.

Proves the three projections are complete and that the loader rejects data the
generated tables could not represent honestly. The drift gate proves the
committed artifacts match a fresh emission; this proves a fresh emission is
worth matching.
"""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import capability_emit as emit  # noqa: E402

_REPO_CAPABILITIES = Path(__file__).resolve().parents[1] / "capabilities"

FAILURES: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)


def expect_error(directory: Path, message: str) -> None:
    try:
        concepts = emit.load_concepts(directory)
        emit.load_formats(directory, {c["id"] for c in concepts})
    except (emit.EmitError, json.JSONDecodeError):
        return
    FAILURES.append(f"accepted invalid data: {message}")


def write(directory: Path, name: str, payload: dict) -> None:
    if name != "concepts.json" and "format" in payload and "ordinal" not in payload:
        payload = payload | {"ordinal": 0}
    (directory / name).write_text(json.dumps(payload))


def minimal(directory: Path) -> None:
    write(
        directory,
        "concepts.json",
        {
            "concepts": [
                {"id": "unknown", "detect": "format", "summary": "unclassified"},
                {"id": "clip.note", "detect": "model", "summary": "note content"},
                {"id": "clip.media", "detect": "model", "summary": "media content"},
            ]
        },
    )
    write(
        directory,
        "fmt.json",
        {
            "format": "fmt",
            "ordinal": 0,
            "enumerator": "Fmt",
            "display_name": "Fmt",
            "writer": False,
            "import": {"clip.note": {"level": "full"}},
            "export": {"clip.note": {"level": "full"}},
        },
    )


def test_enumerator_naming() -> None:
    check(emit.enumerator_for("clip.note") == "ClipNote", "clip.note enumerator")
    check(
        emit.enumerator_for("automation.device-param") == "AutomationDeviceParam",
        "hyphen and dot segments both capitalize",
    )
    check(
        emit.enumerator_for("edit-rate.non-audio") == "EditRateNonAudio",
        "leading hyphenated segment",
    )


def test_projections_are_complete() -> None:
    """Every concept must appear in every projection, for every format."""
    concepts = emit.load_concepts(_REPO_CAPABILITIES)
    formats = emit.load_formats(_REPO_CAPABILITIES, {c["id"] for c in concepts})

    header = emit.emit_concepts(concepts)
    for concept in concepts:
        check(
            f"    {concept['enumerator']} = " in header,
            f"concepts.hpp is missing enumerator {concept['enumerator']}",
        )
        check(
            f'"{concept["id"]}"' in header,
            f"concepts.hpp is missing id {concept['id']}",
        )
    check(f"kConceptCount = {len(concepts)}" in header, "concepts.hpp count")

    tables = emit.emit_tables(concepts, formats)
    # The closed world is materialized: rows = concepts x formats, in both
    # directions. A missing row would be a hole C++ has to interpret.
    for section in ("kImportRows", "kExportRows"):
        start = tables.index(section)
        end = tables.index("};", start)
        body = tables[start:end]
        for concept in concepts:
            check(
                body.count(f"// {concept['id']}\n") == len(formats),
                f"{section} is missing a row for {concept['id']}",
            )

    docs = emit.emit_docs(concepts, formats)
    for concept in concepts:
        check(f"`{concept['id']}`" in docs, f"docs page is missing {concept['id']}")
    for fmt in formats:
        check(fmt["display_name"] in docs, f"docs page is missing {fmt['format']}")


def test_undeclared_rows_become_refusals() -> None:
    """A concept a format omits must generate None on import, Drop on export."""
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        minimal(root)
        concepts = emit.load_concepts(root)
        formats = emit.load_formats(root, {c["id"] for c in concepts})
        tables = emit.emit_tables(concepts, formats)

        check(
            "{ImportLevel::None, \"\", \"\"}, // clip.media" in tables,
            "an undeclared import row must generate ImportLevel::None",
        )
        check(
            "{ExportLevel::Drop, Concept::Unknown, LossClass::Dropped," in tables
            and "// clip.media" in tables,
            "an undeclared export row must generate ExportLevel::Drop",
        )
        check(
            "{ImportLevel::Full, \"\", \"\"}, // clip.note" in tables,
            "a declared import row must generate its level",
        )


def test_format_order_comes_from_ordinals() -> None:
    """Filename order must not become the installed Format ABI."""
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        minimal(root)
        (root / "fmt.json").unlink()
        common = {
            "writer": False,
            "import": {},
            "export": {},
        }
        write(
            root,
            "z-first.json",
            common
            | {
                "format": "first",
                "ordinal": 0,
                "enumerator": "First",
                "display_name": "First",
            },
        )
        write(
            root,
            "a-second.json",
            common
            | {
                "format": "second",
                "ordinal": 1,
                "enumerator": "Second",
                "display_name": "Second",
            },
        )
        concepts = emit.load_concepts(root)
        formats = emit.load_formats(root, {c["id"] for c in concepts})
        check(
            [fmt["format"] for fmt in formats] == ["first", "second"],
            "format order must follow ordinals rather than filenames",
        )
        tables = emit.emit_tables(concepts, formats)
        check(
            tables.index("First = 0") < tables.index("Second = 1"),
            "emitted Format ABI must follow ordinals",
        )


def test_validation_rejects_dishonest_data() -> None:
    cases: list[tuple[str, callable]] = [
        (
            "unknown must be the zero concept",
            lambda d: write(
                d,
                "concepts.json",
                {
                    "concepts": [
                        {"id": "clip.note", "detect": "model", "summary": "s"},
                        {"id": "unknown", "detect": "format", "summary": "s"},
                    ]
                },
            ),
        ),
        (
            "duplicate concept id",
            lambda d: write(
                d,
                "concepts.json",
                {
                    "concepts": [
                        {"id": "unknown", "detect": "format", "summary": "s"},
                        {"id": "clip.note", "detect": "model", "summary": "s"},
                        {"id": "clip.note", "detect": "model", "summary": "s"},
                    ]
                },
            ),
        ),
        (
            "format row naming a concept outside the vocabulary",
            lambda d: write(
                d,
                "fmt.json",
                {
                    "format": "fmt",
                    "enumerator": "Fmt",
                    "display_name": "Fmt",
                    "writer": False,
                    "import": {"clip.telepathy": {"level": "full"}},
                    "export": {},
                },
            ),
        ),
        (
            "import level outside the declared set",
            lambda d: write(
                d,
                "fmt.json",
                {
                    "format": "fmt",
                    "enumerator": "Fmt",
                    "display_name": "Fmt",
                    "writer": False,
                    "import": {"clip.note": {"level": "maybe"}},
                    "export": {},
                },
            ),
        ),
        (
            "degrade without a target concept",
            lambda d: write(
                d,
                "fmt.json",
                {
                    "format": "fmt",
                    "enumerator": "Fmt",
                    "display_name": "Fmt",
                    "writer": False,
                    "import": {},
                    "export": {
                        "clip.note": {
                            "level": "degrade",
                            "loss_class": "flattened",
                            "loss": "x",
                        }
                    },
                },
            ),
        ),
        (
            "a loss without an explanation",
            lambda d: write(
                d,
                "fmt.json",
                {
                    "format": "fmt",
                    "enumerator": "Fmt",
                    "display_name": "Fmt",
                    "writer": False,
                    "import": {},
                    "export": {"clip.note": {"level": "drop", "loss_class": "dropped"}},
                },
            ),
        ),
        (
            "a loss without a class",
            lambda d: write(
                d,
                "fmt.json",
                {
                    "format": "fmt",
                    "enumerator": "Fmt",
                    "display_name": "Fmt",
                    "writer": False,
                    "import": {},
                    "export": {"clip.note": {"level": "drop", "loss": "gone"}},
                },
            ),
        ),
        (
            "enumerator that is not an identifier",
            lambda d: write(
                d,
                "fmt.json",
                {
                    "format": "fmt",
                    "enumerator": "not an identifier",
                    "display_name": "Fmt",
                    "writer": False,
                    "import": {},
                    "export": {},
                },
            ),
        ),
        (
            "missing writer declaration",
            lambda d: write(
                d,
                "fmt.json",
                {
                    "format": "fmt",
                    "enumerator": "Fmt",
                    "display_name": "Fmt",
                    "import": {},
                    "export": {},
                },
            ),
        ),
        (
            "concept without a detect kind",
            lambda d: write(
                d,
                "concepts.json",
                {
                    "concepts": [
                        {"id": "unknown", "summary": "s"},
                        {"id": "clip.note", "detect": "model", "summary": "s"},
                    ]
                },
            ),
        ),
    ]

    for message, mutate in cases:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            minimal(root)
            mutate(root)
            expect_error(root, message)

    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        minimal(root)
        write(
            root,
            "other.json",
            {
                "format": "other",
                "ordinal": 0,
                "enumerator": "Other",
                "display_name": "Other",
                "writer": False,
                "import": {},
                "export": {},
            },
        )
        expect_error(root, "duplicate format ordinal")

    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        minimal(root)
        data = json.loads((root / "fmt.json").read_text())
        data.pop("ordinal")
        (root / "fmt.json").write_text(json.dumps(data))
        expect_error(root, "missing format ordinal")

    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        minimal(root)
        data = json.loads((root / "fmt.json").read_text())
        data["ordinal"] = 1
        write(root, "other.json", data | {"enumerator": "Other"})
        expect_error(root, "duplicate stable format id")

    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        minimal(root)
        data = json.loads((root / "fmt.json").read_text())
        data["ordinal"] = 1
        write(root, "fmt.json", data)
        expect_error(root, "format ordinal gap")

    # No format files at all is an operational error, not an empty matrix.
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        minimal(root)
        (root / "fmt.json").unlink()
        expect_error(root, "no format files")


def test_committed_data_loads() -> None:
    """The repo's own capability data must satisfy every rule above."""
    concepts = emit.load_concepts(_REPO_CAPABILITIES)
    formats = emit.load_formats(_REPO_CAPABILITIES, {c["id"] for c in concepts})
    check(concepts[0]["id"] == "unknown", "committed vocabulary starts with unknown")
    check(len(formats) >= 1, "committed data declares at least one format")


def main() -> int:
    test_enumerator_naming()
    test_projections_are_complete()
    test_undeclared_rows_become_refusals()
    test_format_order_comes_from_ordinals()
    test_validation_rejects_dishonest_data()
    test_committed_data_loads()

    if FAILURES:
        for failure in FAILURES:
            print(f"capability-emit-selftest: {failure}", file=sys.stderr)
        return 1
    print("capability_emit_selftest=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
