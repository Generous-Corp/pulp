#!/usr/bin/env python3
"""Prove the shared Catch2 test PCH reached the generated compile lines.

PulpTestSuite.cmake decides at configure time which Catch2 executables reuse
`pulp-test-pch-cxx20` / `pulp-test-pch-cxx23` and writes its decisions to
`<build>/pulp-test-pch.tsv`. That ledger is a claim; this check reads the
generator's own output (build.ninja, or the per-target flags.make of a
Makefile build) and verifies:

  * every ledger `pch` target compiles with `-include-pch` naming its
    carrier's `cmake_pch.hxx.pch`, and no `skip`/`off` target does;
  * each consumer's `-std=` matches its carrier's (clang rejects a standard
    mismatch loudly, but a gnu++/c++ extensions difference it accepts);
  * every `-D` the carrier's PCH was compiled with is present on the consumer
    (clang accepts a PCH macro the consumer lacks WITHOUT a diagnostic, so
    this is the one mismatch nothing else would ever report);
  * no consumer carries -fno-exceptions / -fno-rtti;
  * every compile that produces a PCH and runs through ccache turns base_dir
    off (`CCACHE_BASEDIR=` in its launcher). A .pch embeds its build tree's
    absolute paths, so a base_dir-relative key would hand one worktree's PCH
    to another (see pulp_ccache_key_on_build_path in Ccache.cmake);
  * named expectations hold (`--expect target=carrier` or `=none`), which
    covers a representative C++20 suite, a C++23 suite, an excluded suite,
    and SDL3-static (whose own PCH is turned off so ccache can store it).

Exit 0 when everything holds, 1 with every violation listed otherwise.
"""
from __future__ import annotations

import argparse
import re
import shlex
import sys
from dataclasses import dataclass, field
from pathlib import Path

LEDGER_NAME = "pulp-test-pch.tsv"
CARRIER_PREFIX = "pulp-test-pch-cxx"
INCOMPATIBLE_FLAGS = ("-fno-exceptions", "-fno-rtti")


@dataclass
class CompileLine:
    target: str
    output: str
    flags: str
    defines: str
    launcher: str = ""

    @property
    def uses_pch(self) -> bool:
        return "-include-pch" in self.flags or "cmake_pch" in self.flags

    @property
    def pch_carrier(self) -> str | None:
        m = re.search(r"CMakeFiles/(" + re.escape(CARRIER_PREFIX) + r"\d+)\.dir/cmake_pch", self.flags)
        return m.group(1) if m else None

    @property
    def std(self) -> str | None:
        m = re.search(r"(?:^|\s)(-std=\S+)", self.flags)
        return m.group(1) if m else None

    def define_set(self) -> set[str]:
        return {tok for tok in shlex.split(self.defines) if tok.startswith("-D")}


@dataclass
class Ledger:
    entries: dict[str, tuple[str, str]] = field(default_factory=dict)  # target -> (status, detail)


def read_ledger(build_dir: Path) -> Ledger:
    path = build_dir / LEDGER_NAME
    if not path.is_file():
        raise SystemExit(f"pch-wiring: ledger missing: {path} (PulpTestSuite.cmake did not run its finalize pass)")
    ledger = Ledger()
    for raw in path.read_text(encoding="utf-8").splitlines():
        if not raw or raw.startswith("#"):
            continue
        parts = raw.split("\t")
        if len(parts) < 2:
            raise SystemExit(f"pch-wiring: malformed ledger line: {raw!r}")
        target, status = parts[0], parts[1]
        detail = parts[2] if len(parts) > 2 else ""
        ledger.entries[target] = (status, detail)
    return ledger


def detect_generator(build_dir: Path) -> str:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        raise SystemExit(f"pch-wiring: not a CMake build dir (no CMakeCache.txt): {build_dir}")
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CMAKE_GENERATOR:INTERNAL="):
            return line.split("=", 1)[1].strip()
    raise SystemExit("pch-wiring: CMAKE_GENERATOR missing from CMakeCache.txt")


_NINJA_BUILD = re.compile(r"^build (?P<outputs>[^:]+):\s*(?P<rule>\S+)")
_NINJA_TARGET_DIR = re.compile(r"CMakeFiles/(?P<target>[^/]+)\.dir/")


def parse_ninja(build_dir: Path) -> list[CompileLine]:
    files = [build_dir / "build.ninja"] + sorted((build_dir / "CMakeFiles").glob("impl-*.ninja"))
    lines: list[CompileLine] = []
    for path in files:
        if not path.is_file():
            continue
        current: dict | None = None
        for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = _NINJA_BUILD.match(raw)
            if m:
                if current:
                    lines.append(CompileLine(**current))
                current = None
                rule = m.group("rule")
                if not re.match(r"^(CXX|C|OBJCXX|OBJC)_COMPILER__", rule):
                    continue
                output = m.group("outputs").split("|")[0].strip().split(" ")[0]
                # Ninja escapes spaces and colons in paths; undo the ones that matter here.
                output = output.replace("$ ", " ").replace("$:", ":")
                t = _NINJA_TARGET_DIR.search(output)
                if not t:
                    continue
                current = {"target": t.group("target"), "output": output, "flags": "", "defines": "",
                           "launcher": ""}
                continue
            if current is not None and raw.startswith("  "):
                key, _, value = raw.strip().partition(" = ")
                if key == "FLAGS":
                    current["flags"] = value
                elif key == "DEFINES":
                    current["defines"] = value
                elif key == "LAUNCHER":
                    current["launcher"] = value
        if current:
            lines.append(CompileLine(**current))
    return lines


def parse_makefiles(build_dir: Path) -> list[CompileLine]:
    lines: list[CompileLine] = []
    for flags_make in build_dir.rglob("flags.make"):
        target = flags_make.parent.name
        if not target.endswith(".dir"):
            continue
        target = target[: -len(".dir")]
        per_lang: dict[str, dict[str, str]] = {}
        for raw in flags_make.read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.match(r"^(CXX|C|OBJCXX|OBJC)_(FLAGS|DEFINES) = (.*)$", raw)
            if m:
                per_lang.setdefault(m.group(1), {})[m.group(2)] = m.group(3)
        for lang, kv in per_lang.items():
            lines.append(CompileLine(
                target=target,
                output=f"{flags_make.parent.relative_to(build_dir)}/<{lang}>",
                flags=kv.get("FLAGS", ""),
                defines=kv.get("DEFINES", ""),
            ))
    return lines


def parse_compile_lines(build_dir: Path) -> list[CompileLine]:
    generator = detect_generator(build_dir)
    if "Ninja" in generator:
        return parse_ninja(build_dir)
    if "Makefiles" in generator:
        return parse_makefiles(build_dir)
    raise SystemExit(f"pch-wiring: unsupported generator for this check: {generator}")


_BASEDIR_OFF = re.compile(r"(?:^|\s)CCACHE_BASEDIR=(?:\s|$)")


def pch_producers(build_dir: Path) -> list[tuple[str, str]]:
    """(label, launcher-and-command) for every compile that emits a PCH."""
    if "Ninja" in detect_generator(build_dir):
        return [(line.output, line.launcher) for line in parse_ninja(build_dir)
                if "-emit-pch" in line.flags]
    out: list[tuple[str, str]] = []
    for build_make in build_dir.rglob("build.make"):
        for raw in build_make.read_text(encoding="utf-8", errors="replace").splitlines():
            # Skip the `-x c++-header -E` / `-S` preprocess and assembly
            # helper rules CMake writes next to the real compile.
            if "-emit-pch" in raw and not re.search(r"-x\s+\S+-header\s+-[ES]\s", raw):
                out.append((str(build_make.relative_to(build_dir)), raw.strip()))
    return out


def check_pch_ccache_keys(build_dir: Path) -> tuple[int, list[str]]:
    """PCH compiles through ccache must key on the absolute build path."""
    producers = pch_producers(build_dir)
    problems = [
        f"{label}: produces a PCH through ccache without CCACHE_BASEDIR= in its launcher; "
        "a base_dir-relative key serves this tree's .pch (which embeds its absolute paths) "
        "to other worktrees -- call pulp_ccache_key_on_build_path() on the target"
        for label, cmd in producers
        if "ccache" in cmd and not _BASEDIR_OFF.search(cmd)
    ]
    return len(producers), problems


def group_by_target(lines: list[CompileLine]) -> dict[str, list[CompileLine]]:
    out: dict[str, list[CompileLine]] = {}
    for line in lines:
        out.setdefault(line.target, []).append(line)
    return out


def carrier_pch_line(by_target: dict[str, list[CompileLine]], carrier: str) -> CompileLine | None:
    for line in by_target.get(carrier, []):
        if line.output.endswith("cmake_pch.hxx.pch") or line.output.endswith("<CXX>"):
            return line
    return None


def check(build_dir: Path, option_on: bool, expectations: dict[str, str]) -> list[str]:
    ledger = read_ledger(build_dir)
    by_target = group_by_target(parse_compile_lines(build_dir))
    problems: list[str] = []

    pch_targets = {t for t, (s, _) in ledger.entries.items() if s == "pch"}
    if option_on and not pch_targets:
        problems.append("PULP_TEST_PCH=ON but the ledger records no target reusing a carrier")
    if not option_on and pch_targets:
        problems.append(f"PULP_TEST_PCH=OFF but the ledger records {len(pch_targets)} target(s) reusing a carrier")

    for target, (status, detail) in sorted(ledger.entries.items()):
        lines = by_target.get(target)
        if not lines:
            problems.append(f"{target}: ledger lists it but no compile line was generated for it")
            continue
        if status != "pch":
            for line in lines:
                if line.uses_pch:
                    problems.append(f"{target}: ledger says {status} ({detail}) but {line.output} compiles with a PCH")
            continue
        carrier = detail
        carrier_line = carrier_pch_line(by_target, carrier)
        if carrier_line is None:
            problems.append(f"{target}: carrier {carrier} has no PCH compile line")
            continue
        for line in lines:
            if line.pch_carrier != carrier:
                problems.append(
                    f"{target}: {line.output} should reuse {carrier} but its flags name "
                    f"{line.pch_carrier or 'no PCH'}")
                continue
            if line.std != carrier_line.std:
                problems.append(f"{target}: -std {line.std} differs from carrier {carrier} {carrier_line.std}")
            for flag in INCOMPATIBLE_FLAGS:
                if flag in line.flags.split():
                    problems.append(f"{target}: {flag} on a PCH consumer")
            missing = carrier_line.define_set() - line.define_set()
            if missing:
                problems.append(
                    f"{target}: carrier {carrier} was built with {sorted(missing)} which the consumer lacks "
                    "(clang would accept that silently)")

    n_producers, key_problems = check_pch_ccache_keys(build_dir)
    problems.extend(key_problems)
    if option_on and pch_targets and n_producers == 0:
        problems.append("targets reuse a carrier but no PCH-producing compile line was found")

    for target, expected in sorted(expectations.items()):
        lines = by_target.get(target)
        if not lines:
            problems.append(f"expectation {target}={expected}: no compile line for {target}")
            continue
        if expected == "none":
            for line in lines:
                if line.uses_pch:
                    problems.append(f"expectation {target}=none: {line.output} compiles with a PCH")
            if target in ledger.entries and ledger.entries[target][0] == "pch":
                problems.append(f"expectation {target}=none: ledger says pch")
            continue
        if not option_on:
            continue  # carrier expectations only apply when the option is on
        entry = ledger.entries.get(target)
        if entry is None or entry[0] != "pch" or entry[1] != expected:
            problems.append(f"expectation {target}={expected}: ledger says {entry}")
        for line in lines:
            if line.pch_carrier != expected:
                problems.append(f"expectation {target}={expected}: {line.output} names {line.pch_carrier or 'no PCH'}")
    return problems


def summarize(build_dir: Path) -> str:
    ledger = read_ledger(build_dir)
    counts: dict[str, int] = {}
    for _, (status, detail) in ledger.entries.items():
        key = f"{status}:{detail}" if status == "pch" else f"{status}:{detail.split(':', 1)[0]}"
        counts[key] = counts.get(key, 0) + 1
    return "\n".join(f"  {k:48s} {v}" for k, v in sorted(counts.items(), key=lambda kv: -kv[1]))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", required=True, type=Path)
    ap.add_argument("--option", choices=("ON", "OFF"), required=True,
                    help="the configured value of PULP_TEST_PCH")
    ap.add_argument("--expect", action="append", default=[], metavar="TARGET=CARRIER|none",
                    help="assert a target reuses the named carrier (or none)")
    ap.add_argument("--summary", action="store_true", help="print the ledger tally")
    args = ap.parse_args(argv)

    expectations: dict[str, str] = {}
    for item in args.expect:
        target, _, carrier = item.partition("=")
        if not target or not carrier:
            ap.error(f"--expect wants TARGET=CARRIER|none, got {item!r}")
        expectations[target] = carrier

    build_dir = args.build_dir.resolve()
    problems = check(build_dir, args.option == "ON", expectations)
    if args.summary or problems:
        print(f"pch-wiring: ledger tally for {build_dir}:\n{summarize(build_dir)}")
    if problems:
        print(f"pch-wiring: {len(problems)} problem(s):", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1
    n = len({t for t, (s, _) in read_ledger(build_dir).entries.items() if s == "pch"})
    n_producers, _ = check_pch_ccache_keys(build_dir)
    print(f"pch-wiring: OK ({n} test executables reuse a carrier; {n_producers} PCH compile(s) "
          f"keyed on their build path; {len(expectations)} expectations hold)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
