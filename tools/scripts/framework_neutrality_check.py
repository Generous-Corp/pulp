#!/usr/bin/env python3
"""Keep Pulp's own source free of foreign-framework vocabulary.

Pulp is MIT, public, and reimplements nothing. A capability lands because a
Pulp user wants it, described in Pulp's terms — never as "the thing framework
X calls Y". Two reasons, both load-bearing:

  1. A capability that can only be explained by pointing at another framework
     is a compatibility shim wearing a feature's clothes. The rule is a design
     check: if the doc-comment needs a foreign name to make sense, the API is
     wrong.

  2. Source that cites another framework's internals as its rationale — "mirrors
     the X class of the same name", "usage mirrors X" — reads as a written
     admission of derivation regardless of how the code was actually written.

The check has TWO halves, and the second is the one that catches the deeper
problem:

  * PROSE — a foreign framework named in a comment, docstring, or identifier.
  * NAMES — a foreign framework's CLASS NAME adopted into Pulp's own API. A
    comment is a liability; an adopted type name is the liability shipped in
    the public headers, where every downstream user inherits it.

Translation vocabulary (`SomeFramework::Widget -> pulp::view::Widget`) is data,
and it belongs in the importer that owns it — not here.

DELIBERATELY NOT FLAGGED
------------------------
* Plug-in format APIs Pulp implements (`IPlugView`, `IPluginFactory`,
  `IComponent`, ...). Those are Steinberg's VST3 interface names and Pulp
  speaks them by necessity. Word boundaries keep them out of the net.
* Deprecated aliases (`using OldName [[deprecated(...)]] = NewName;`). Naming a
  foreign name in order to POINT AWAY from it is the mechanism that lets a
  rename ship without breaking downstream code. Any line containing
  `[[deprecated` is skipped.
* `EXEMPT_SUBSTRINGS` below — each entry has its reason recorded inline. The
  three classes are: clean-room provenance (host-quirks), the importer's own
  denylist (removing the strings would DISABLE the check that rejects vendored
  foreign code), and free-form user input echoed in a CLI fixture.
* Out of scope entirely (not scanned): `docs/`, `.agents/`, `tools/`,
  `.github/`, `README.md`, `CHANGELOG.md`. Those legitimately DISCUSS interop
  with other frameworks — `docs/guides/coming-from-juce.md` exists precisely to
  tell those users how to adopt Pulp. And `planning/` + `external/`, which are
  not Pulp's source.

Usage:
    python3 tools/scripts/framework_neutrality_check.py             # scan (exit 1 on a finding)
    python3 tools/scripts/framework_neutrality_check.py --mode=hint # advisory (always exit 0)
    python3 tools/scripts/framework_neutrality_check.py --selftest  # prove the gate can fail
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Pulp's own source. Everything Pulp ships or builds from, minus the surfaces
# whose job is to TALK ABOUT other frameworks (see the docstring).
SCANNED_ROOTS = [
    "core",
    "inspect",
    "ship",
    "packages",
    "experimental",
    "test",
    "apple",
]

# Directories never worth walking (third-party trees and build output that can
# appear inside a scanned root).
SKIP_DIRS = {
    "node_modules", "target", "build", "dist", ".git", "__pycache__",
    "Pods", ".venv", "venv",
}

SCANNED_SUFFIXES = {
    ".h", ".hpp", ".c", ".cc", ".cpp", ".mm", ".m", ".swift",
    ".js", ".mjs", ".ts", ".tsx", ".rs",
    ".txt", ".cmake", ".supp",
}

# Paths whose foreign-framework references are the POINT. Deleting them would be
# a bug, not a cleanup.
EXEMPT_SUBSTRINGS = [
    # Clean-room provenance. The prior-art citation IS the audit trail the
    # Reference-Lineage trailer policy requires; removing it destroys the
    # provenance it exists to record. Covers the catalog, its schema, and the
    # tests that pin which rows are LessonOnly (host classifier included — it is
    # what feeds HostQuirks).
    "format/host_quirks/",
    "host-quirks.json",
    "test/test_host_quirks.cpp",
    "test/test_host_version.cpp",
    # The importer's own DENYLIST. These tests assert that emitted output is
    # REJECTED when it contains vendored foreign framework code. The literal
    # framework names are the thing being screened for — strip them and the
    # check silently passes everything.
    "test/test_cli_import.cpp",
    "test/test_cli_import_emit.cpp",
    "test/test_cli_import_terms.cpp",
    # `pulp pkg suggest --alternative <name>` takes a free-form framework name
    # from the user and suggests Pulp packages that cover the same ground. The
    # arg-parsing test has to pass SOME name; that value is user input, not
    # Pulp vocabulary.
    "experimental/pulp-rs/src/cmd/pkg.rs",
    # The downstream-consumer manifest names the real interop repos Pulp builds
    # against in CI (pulp-embed-*). Those are repository identifiers.
    "test/fixtures/refactor-baselines/",
]

# ── PROSE: a foreign framework named in Pulp's source ───────────────────────
# Word-boundary anchored so VST3's `IPlugView` / `IPluginFactory` do not match
# `iplug`, and so `juice` / `prejudice` do not match `juce`.
PROSE_PATTERNS = [
    (re.compile(r"\bjuce\b", re.I), "foreign framework named in Pulp source"),
    (re.compile(r"\biplug2?\b", re.I), "foreign framework named in Pulp source"),
    (re.compile(r"\bvstgui\b", re.I), "foreign framework named in Pulp source"),
    (re.compile(r"\bhise\b", re.I), "foreign framework named in Pulp source"),
    (re.compile(r"\bwdl\b", re.I), "foreign framework named in Pulp source"),
    (re.compile(r"\blook\s*and\s*feel\b", re.I), "foreign skinning concept"),
    (re.compile(r"\bAPVTS\b"), "foreign parameter-tree concept"),
    (re.compile(r"\bdraw(Rotary|Linear)Slider\b"), "foreign draw-hook name"),
    (re.compile(r"\baddAndMakeVisible\b"), "foreign view-tree call"),
]

# ── NAMES: a foreign class name adopted into Pulp's own API ─────────────────
# This is the half that catches the deeper problem. Each entry names the Pulp
# type to use instead, so the failure message is actionable rather than
# accusatory.
NAME_PATTERNS = [
    (re.compile(r"\bNormalisableRange\b"),
     "foreign class name — use pulp::state::SkewedRange"),
    (re.compile(r"\bAbstractFifo\b"),
     "foreign class name — use pulp::runtime::SpscRingIndex"),
    (re.compile(r"\bExtensionsVisitor\b"),
     "foreign class name — use pulp::host::NativeHandleVisitor"),
    (re.compile(r"\bLookAndFeel\w*"),
     "foreign class name — Pulp skins through themes + tokens, not a L&F object"),
    (re.compile(r"\bSafePointer\b"),
     "foreign class name — use a weak handle / liveness token"),
    (re.compile(r"\bAudioProcessorValueTreeState\b"),
     "foreign class name — use pulp::state::StateStore"),
    (re.compile(r"\bAudioProcessorEditor\b"),
     "foreign class name — use pulp::view::View + Processor::create_view()"),
    (re.compile(r"\bValueTree\b"),
     "foreign class name — use pulp::state::StateTree"),
    (re.compile(r"\bChangeBroadcaster\b"),
     "foreign class name — use a listener list / callback"),
    (re.compile(r"\bReferenceCountedObject\b"),
     "foreign class name — use std::shared_ptr"),
]

PATTERNS = [(p, w, "prose") for p, w in PROSE_PATTERNS] + \
           [(p, w, "name") for p, w in NAME_PATTERNS]

# A rename ships with an alias that names the OLD spelling so downstream keeps
# compiling. That is the one legitimate way a foreign name may appear.
DEPRECATED_ALIAS = "[[deprecated"

SUGGESTION = """
  PROSE — describe the behaviour, not its ancestry. Instead of:
      // Usage mirrors <Framework>'s AbstractFifo
  write what it actually does, in units a Pulp reader can act on:
      /// Tracks read/write cursors in a ring buffer the CALLER owns; returns
      /// two index ranges because a run of items may wrap the end of the array.

  NAMES — rename the type to what it does, and keep a deprecated alias so
  downstream code keeps compiling:
      using OldName [[deprecated("renamed to NewName")]] = NewName;

  If the API only makes sense as "the <Framework> thing", it is a shim, not a
  feature — redesign it. Framework->Pulp translation tables belong in the
  importer that owns them.
""".rstrip()


def is_exempt(rel: str) -> bool:
    return any(x in rel for x in EXEMPT_SUBSTRINGS)


def iter_files(root_dir: Path):
    for path in root_dir.rglob("*"):
        if not path.is_file() or path.suffix not in SCANNED_SUFFIXES:
            continue
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        yield path


def scan(repo: Path = REPO) -> list[tuple[str, int, str, str, str]]:
    hits: list[tuple[str, int, str, str, str]] = []
    for root in SCANNED_ROOTS:
        base = repo / root
        if not base.is_dir():
            continue
        for path in iter_files(base):
            rel = str(path.relative_to(repo))
            if is_exempt(rel):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for lineno, line in enumerate(text.splitlines(), 1):
                if DEPRECATED_ALIAS in line:
                    continue
                for pattern, why, kind in PATTERNS:
                    if pattern.search(line):
                        hits.append((rel, lineno, line.strip()[:100], why, kind))
                        break
    return hits


def report(hits) -> None:
    print("framework-neutrality: foreign-framework vocabulary in Pulp's own source\n")
    for rel, lineno, line, why, kind in hits:
        print(f"  [{kind}] {rel}:{lineno}: {why}")
        print(f"      {line}")
    print(SUGGESTION)
    prose = sum(1 for h in hits if h[4] == "prose")
    names = sum(1 for h in hits if h[4] == "name")
    print(f"\n{len(hits)} finding(s) — {prose} prose, {names} adopted name(s).")


def selftest() -> int:
    """Prove the gate can FAIL. A gate that cannot fail is not a gate.

    Builds a throwaway tree shaped like the repo and asserts the checker
    flags a seeded violation of EACH half (prose + adopted name), stays quiet
    on clean source, and honours both the file exemptions and the
    deprecated-alias line exemption.
    """
    failures = 0

    def case(name: str, files: dict[str, str], expect_hits: int) -> None:
        nonlocal failures
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for rel, content in files.items():
                p = root / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(content, encoding="utf-8")
            found = scan(root)
            ok = len(found) == expect_hits
            if not ok:
                failures += 1
            print(f"[{'ok' if ok else 'FAIL'}] {name} "
                  f"(expect {expect_hits}); found={len(found)}")
            if not ok:
                for rel, lineno, line, why, kind in found:
                    print(f"        · [{kind}] {rel}:{lineno} {line}")

    case("clean source is silent",
         {"core/view/src/knob.cpp": "// Draws a knob. Skew of 1.0 is linear.\n"},
         0)

    case("seeded PROSE violation is flagged",
         {"core/view/src/knob.cpp": "// Mirrors the JUCE class of the same name.\n"},
         1)

    case("seeded ADOPTED-NAME violation is flagged",
         {"core/state/include/pulp/state/parameter.hpp":
          "struct NormalisableRange { float min, max; };\n"},
         1)

    case("seeded violation in a non-C++ source file is flagged",
         {"core/view/js/web-compat-element.js": "// as JUCE's exporter emits\n",
          "packages/pulp-react/src/types.ts": "/// a LookAndFeel object\n"},
         2)

    case("deprecated alias naming the old spelling is allowed",
         {"core/runtime/include/pulp/runtime/spsc_ring_index.hpp":
          'using AbstractFifo [[deprecated("renamed to SpscRingIndex")]] = SpscRingIndex;\n'},
         0)

    case("host-quirks provenance is exempt",
         {"core/format/include/pulp/format/host_quirks/cubase.hpp":
          "// Cross-checked against the iPlug2 quirks audit.\n"},
         0)

    case("importer denylist literals are exempt",
         {"test/test_cli_import.cpp": '  "juce", "iplug", "steinberg", "wdl",\n'},
         0)

    case("out-of-scope trees are not scanned",
         {"docs/guides/coming-from-juce.md": "Moving a JUCE plugin to Pulp\n",
          "tools/import/terms.cpp": '// map JUCE::Slider -> pulp::view::Slider\n'},
         0)

    print()
    if failures:
        print(f"framework-neutrality selftest: {failures} case(s) FAILED")
        return 1
    print("framework-neutrality selftest: all cases pass")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mode", choices=["hint", "report"], default="report",
                    help="report (default) exits 1 on a finding; hint always exits 0")
    ap.add_argument("--selftest", action="store_true",
                    help="run the checker against seeded fixtures and exit")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    hits = scan()
    if not hits:
        print(f"framework-neutrality: clean ({', '.join(SCANNED_ROOTS)})")
        return 0

    report(hits)
    return 0 if args.mode == "hint" else 1


if __name__ == "__main__":
    sys.exit(main())
