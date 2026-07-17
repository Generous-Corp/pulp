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
* Out of scope entirely (not scanned): `docs/` — EXCEPT `docs/reports/`, see
  below — plus `.agents/`, `.github/`, `README.md`, `CHANGELOG.md`. Those
  legitimately DISCUSS interop with other frameworks —
  `docs/guides/coming-from-juce.md` exists precisely to tell those users how to
  adopt Pulp. And `planning/` + `external/`, which are not Pulp's source.

WHY `docs/reports/` IS SCANNED AND THE REST OF `docs/` IS NOT
-------------------------------------------------------------
The three kinds of doc under `docs/` have different jobs, and only one of them
has a reason to name a framework:

  * `docs/guides/` MIGRATES. A migration guide must name what you migrate FROM,
    or it cannot do its job.
  * `docs/reference/` COMPARES APIs and records attribution.
  * `docs/reports/` is a WORK RECORD — what Pulp hardened, and the evidence. It
    is public, but it is written for us. That combination is exactly how a
    private-shaped document ends up on a public surface: a report has no
    audience reason to name a framework, so a name in one is a comparison that
    leaked rather than a fact a reader needs.

So the seam is drawn at `docs/reports/`, not at `docs/`. Scanning all of `docs/`
would flag ~78 legitimate hits, 55 of them in the two files whose entire purpose
is to name what you are coming from.

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
    "tools",
    # Work records only — NOT all of `docs/`. See the docstring for the seam.
    "docs/reports",
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
    # Schemas, fixtures, and the tooling that emits them leak just as loudly as
    # C++ does — a framework named in a JSON schema or a README is still the SDK
    # announcing which frameworks it anticipates. (docs/ is deliberately NOT a
    # scanned root: a migration guide is allowed to name what you migrate FROM.)
    ".json", ".md", ".py",
}

# Paths whose foreign-framework references are the POINT. Deleting them would be
# a bug, not a cleanup.
EXEMPT_SUBSTRINGS = [
    # The host-quirk catalog names the DAWs it accommodates, because a quirk row
    # is meaningless without the host it applies to. Stripping those names would
    # not make the catalog neutral; it would make it unusable. Covers the
    # catalog, its schema, and the tests pinning which rows are advisory-only
    # (the host classifier included — it is what feeds HostQuirks).
    "format/host_quirks/",
    "host-quirks.json",
    "test/test_host_quirks.cpp",
    "test/test_host_version.cpp",
    # THIS FILE. A gate that screens for foreign-framework names has to contain
    # them; that is unavoidable and it is the one place they are allowed to live.
    # It is lint tooling, not SDK source: it is never installed by
    # `cmake --install` and never reaches a user's project.
    "tools/scripts/framework_neutrality_check.py",
    # The build/lint tooling that has to name a foreign toolchain to integrate
    # with it (an optional SDK the user supplies themselves), plus the vendored
    # third-party trees under tools/.
    "tools/cmake/",
    "tools/deps/",
    # MIGRATION AND PACKAGING GUIDANCE — a different thing from the import
    # surface, and legitimately allowed to name a framework.
    #
    # The line this gate actually draws: naming a framework in advice about
    # MOVING to Pulp ("if your project already uses libMTSClient.h, map these
    # calls") tells a user something useful and implies nothing about what Pulp
    # ships. Naming one anywhere in the IMPORT surface (tools/import/, tools/cli/)
    # announces which frameworks the SDK anticipates an importer for — and that
    # announcement is the leak, whether or not the importer exists. Those two
    # trees stay strictly neutral; these do not have to.
    "tools/packages/registry.json",
    "tools/validation/",
    "tools/scripts/verify_downstream_",
    "tools/scripts/cmajor_external.py",
    "tools/scripts/test_cmajor_external_extra.py",
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

SUGGESTION = """
  PROSE — describe the behavior, not its ancestry. Instead of:
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

    # Policy: an alias to a foreign name is NOT allowed. It preserves the foreign
    # name in the tree, which is the exact red flag this gate removes. Rename every
    # caller instead of leaving a compatibility alias.
    case("a deprecated alias to a foreign name is flagged",
         {"core/runtime/include/pulp/runtime/spsc_ring_index.hpp":
          'using AbstractFifo [[deprecated("renamed to SpscRingIndex")]] = SpscRingIndex;\n'},
         1)

    case("host-quirks provenance is exempt",
         {"core/format/include/pulp/format/host_quirks/cubase.hpp":
          "// Cross-checked against the iPlug2 quirks audit.\n"},
         0)

    # POLICY CHANGE: a hardcoded denylist of framework names is no longer allowed
    # ANYWHERE in the tree, tests included. The importer's real denylist is
    # runtime DATA carried by the add-on tool's registry descriptor
    # (denylist_from_known_frameworks), so the SDK never needs to spell a
    # framework name to screen for one. A test that hardcodes the list is both a
    # duplicate source of truth and the very leak this gate exists to prevent.
    case("hardcoded denylist literals are flagged, tests included",
         {"test/test_cli_import.cpp": '  "juce", "iplug", "steinberg", "wdl",\n'},
         1)

    # tools/ IS scanned now — the CLI is shipped SDK surface and must be as
    # neutral as core. docs/ is still out of scope: a migration guide is allowed
    # to name what you are migrating FROM.
    case("the CLI tree is scanned; docs are not",
         {"docs/guides/coming-from-juce.md": "Moving a JUCE plugin to Pulp\n",
          "tools/import/terms.cpp": '// map JUCE::Slider -> pulp::view::Slider\n'},
         1)

    # The `docs/` exemption stops at `docs/reports/`. A report is a work record
    # with no audience reason to name a framework, so a name in one is leaked
    # comparison material. Pinning BOTH halves here means widening the scope back
    # out to all of `docs/` — or narrowing it away from reports — has to be a
    # deliberate edit to this case, not a quiet drift in SCANNED_ROOTS.
    case("docs/reports is scanned; the rest of docs is not",
         {"docs/reports/some-hardening-plan.md":
          "gaps identified by comparing Pulp with JUCE and iPlug2\n",
          "docs/guides/coming-from-juce.md": "Moving a JUCE plugin to Pulp\n",
          "docs/reference/licensing.md": "| iPlug2 | zlib-like | attribution |\n"},
         1)

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
