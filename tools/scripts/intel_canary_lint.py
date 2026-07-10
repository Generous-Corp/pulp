#!/usr/bin/env python3
"""intel_canary_lint.py — cross-arch (Intel / x86_64) portability lint.

This is the Tier-0 "Intel canary": a sub-second static check that catches the
most common way a macOS-arm64-only assumption regresses back into the tree.
It is deliberately a CANARY, not a substitute for an actual Intel build+test
(see docs/guides/intel-support.md for the full tiering and the honest
catch/miss list).

It runs in two places:
  * at CMake configure time when `-DPULP_INTEL_CANARY=ON` (pulp opts in via a
    repo variable; external cloners default OFF and pay nothing), and
  * as a fast step in the existing ARM macOS CI job (`--mode=changed`).

It flags exactly five classes of arch regression:

  (1) Raw NEON intrinsics / `arm_neon.h` used in `core/` OUTSIDE an
      `__aarch64__` / `__ARM_NEON` guard — i.e. code that only compiles on ARM.
  (2) A preprocessor `#if` chain that gates SIMD code on an ARM macro with no
      `__x86_64__` / `__SSE*` sibling branch AND no `#else` fallback — the
      "dropped the SSE/scalar fallback" pattern that fails to compile on x86.
  (3) A hardcoded `darwin-arm64` / `mac-arm64` / `aarch64` asset or arch string
      in `tools/cmake/**` or `tools/scripts/fetch_*` that is not covered by the
      allowlist (the file records why each legitimate occurrence is arch-aware).
  (4) `CMAKE_SYSTEM_PROCESSOR` (the HOST arch) consulted for an Apple TARGET-arch
      decision in a `tools/cmake/**` file that never consults
      `CMAKE_OSX_ARCHITECTURES` (the TARGET arch) — the exact wiring bug that
      kept Pulp arm64-only.
  (5) A hardcoded `CMAKE_OSX_ARCHITECTURES=arm64` (arm64-only, no `x86_64`) in
      `tools/cmake/**`.

Design contract: this lint MUST return clean (`--mode=tree`) on a healthy tree.
If it does not, that is a real portability regression to fix at the source — do
NOT weaken the lint to make it pass. Genuinely-legitimate, arch-aware
occurrences that superficially match a token are recorded, with a reason, in
`tools/scripts/intel_canary_allowlist.txt`.

Self-test: `python3 tools/scripts/test_intel_canary_lint.py`.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


# ── Finding model ────────────────────────────────────────────────────────────
@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    klass: int
    message: str
    text: str = ""

    def render(self) -> str:
        return (
            f"{self.path}:{self.line}: [intel-canary C{self.klass}] "
            f"{self.message}\n    | {self.text.strip()}"
        )


CLASS_TITLES = {
    1: "raw NEON intrinsics / arm_neon.h outside an ARM guard in core/",
    2: "ARM-gated SIMD #if chain with no x86 sibling branch and no #else fallback",
    3: "hardcoded darwin-arm64 / mac-arm64 / aarch64 (not allowlisted)",
    4: "CMAKE_SYSTEM_PROCESSOR used for an Apple target-arch decision "
       "(use CMAKE_OSX_ARCHITECTURES)",
    5: "hardcoded CMAKE_OSX_ARCHITECTURES=arm64 (arm64-only)",
}


# ── Regexes ──────────────────────────────────────────────────────────────────
_ARM_MACRO = re.compile(
    r"\b__aarch64__\b|\b__ARM_NEON(?:__)?\b|\b__arm__\b|\b__arm64__\b"
    r"|\b_M_ARM64\b|\b_M_ARM\b"
)
_X86_MACRO = re.compile(
    r"\b__x86_64__\b|\b__amd64__\b|\b_M_X64\b|\b_M_AMD64\b|\b__i386__\b"
    r"|\b_M_IX86\b|\b__SSE\w*\b|\b__AVX\w*\b"
)
_NEON_INCLUDE = re.compile(r'#\s*include\s*[<"]arm_neon\.h[>"]')
# NEON intrinsics carry a canonical lane-type suffix (`_f32`, `_s16`, `_u8`,
# `_p64`, …). Requiring that suffix keeps the token from matching identifiers
# and strings that merely start with a NEON-looking prefix — critically "vst3"
# / "vst1" (the VST3 plugin format and its variants), which are NOT NEON.
_NEON_TYPE = (
    r"(?:f16|f32|f64|s8|s16|s32|s64|u8|u16|u32|u64|p8|p16|p64|bf16)"
)
_NEON_INTRINSIC = re.compile(
    r"\bv(?:ld|st)[1-4]q?(?:_lane|_dup|_x[2-4])?_" + _NEON_TYPE + r"\b"
    r"|\bv[a-z][a-z0-9]*q_" + _NEON_TYPE + r"\b"
)
# SIMD tokens whose presence in a conditional body marks the block as "does SIMD".
_SIMD_BODY = re.compile(
    r"#\s*include\s*[<\"]arm_neon\.h[>\"]"
    r"|\bv(?:ld|st)[1-4]q?(?:_lane|_dup|_x[2-4])?_" + _NEON_TYPE + r"\b"
    r"|\bv[a-z][a-z0-9]*q_" + _NEON_TYPE + r"\b"
    r"|\b_mm_\w+|\b_mm256_\w+|\b_mm512_\w+|__builtin_ia32_\w+"
    r"|\b(?:pmmintrin|xmmintrin|emmintrin|immintrin|smmintrin)\b"
)

_ARCH_STRING = re.compile(r"darwin-arm64|mac-arm64|\baarch64\b")
_SYSTEM_PROCESSOR_READ = re.compile(
    r"\$\{CMAKE_SYSTEM_PROCESSOR\}"
    r"|\bCMAKE_SYSTEM_PROCESSOR\s+(?:MATCHES|STREQUAL|EQUAL|IN_LIST)"
    r"|\(\s*CMAKE_SYSTEM_PROCESSOR\b"
)
_SYSTEM_PROCESSOR_SET = re.compile(r"\bset\s*\(\s*CMAKE_SYSTEM_PROCESSOR\b", re.I)
_OSX_ARCH_SET = re.compile(
    r"\bset\s*\(\s*CMAKE_OSX_ARCHITECTURES\s+(.*)", re.I
)
_OSX_ARCH_DFLAG = re.compile(r"-D\s*CMAKE_OSX_ARCHITECTURES=(\S+)")
_APPLE_CONTEXT = re.compile(r"\bAPPLE\b|Darwin|darwin|\bmacos\b|mac-|\bosx\b|OSX", re.I)
_OSX_ARCH_ANY = re.compile(r"CMAKE_OSX_ARCHITECTURES")

_C_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".mm", ".m"}


# ── Scope predicates ─────────────────────────────────────────────────────────
def is_core_source(rel: str) -> bool:
    return rel.startswith("core/") and Path(rel).suffix in _C_EXTS


def is_cmake_or_fetch(rel: str) -> bool:
    if rel.startswith("tools/cmake/"):
        return True
    return rel.startswith("tools/scripts/") and Path(rel).name.startswith("fetch_")


def is_cmake(rel: str) -> bool:
    return rel.startswith("tools/cmake/")


def _is_comment_line(line: str) -> bool:
    stripped = line.lstrip()
    return stripped.startswith("#") or stripped.startswith("//")


# ── Allowlist ────────────────────────────────────────────────────────────────
class Allowlist:
    """Path + line-substring exemptions.

    Each entry is ``<repo-relative-path> :: <substring>``. A finding is exempt
    when its file matches the entry path and the entry substring appears in the
    finding's line text. Substrings (not line numbers) keep entries stable as
    files shift, and force each exemption to document the specific construct.
    """

    def __init__(self, entries: list[tuple[str, str]]):
        self._entries = entries

    @classmethod
    def load(cls, path: Path) -> "Allowlist":
        entries: list[tuple[str, str]] = []
        if not path.is_file():
            return cls(entries)
        for raw in path.read_text(encoding="utf-8").splitlines():
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if "::" not in line:
                continue
            file_part, sub = line.split("::", 1)
            entries.append((file_part.strip(), sub.strip()))
        return cls(entries)

    def allows(self, rel_path: str, line_text: str) -> bool:
        for file_part, sub in self._entries:
            if file_part != rel_path:
                continue
            if sub and sub in line_text:
                return True
        return False


# ── C/C++/ObjC analysis (classes 1 & 2) ──────────────────────────────────────
def _cond_of_directive(kind: str, rest: str) -> str:
    rest = rest.strip()
    if kind in ("ifdef",):
        return f"defined({rest})"
    if kind in ("ifndef",):
        return f"!defined({rest})"
    return rest


def find_neon_and_arm_simd(rel_path: str, text: str) -> list[Finding]:
    """Classes 1 and 2 for a single C-family source file."""
    findings: list[Finding] = []
    lines = text.splitlines()

    # ── Class 1: NEON usage outside an ARM guard ────────────────────────────
    # Stack of the *current* branch condition governing each open conditional.
    guard_stack: list[str] = []
    for idx, line in enumerate(lines, start=1):
        stripped = line.lstrip()
        m = re.match(r"#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", stripped)
        if m:
            kind, rest = m.group(1), m.group(2)
            if kind in ("if", "ifdef", "ifndef"):
                guard_stack.append(_cond_of_directive(kind, rest))
            elif kind == "elif":
                if guard_stack:
                    guard_stack[-1] = rest.strip()
            elif kind == "else":
                if guard_stack:
                    guard_stack[-1] = "__ELSE__"
            elif kind == "endif":
                if guard_stack:
                    guard_stack.pop()
            continue
        arm_guarded = any(_ARM_MACRO.search(c) for c in guard_stack)
        if arm_guarded:
            continue
        if _NEON_INCLUDE.search(line) or _NEON_INTRINSIC.search(line):
            findings.append(
                Finding(
                    rel_path,
                    idx,
                    1,
                    "NEON usage is not inside an __aarch64__ / __ARM_NEON guard; "
                    "it will not compile on x86_64",
                    line,
                )
            )

    # ── Class 2: ARM-gated SIMD chain with no x86 sibling and no #else ───────
    @dataclass
    class _Chain:
        conds: list[str]
        has_else: bool
        body: list[str]
        start_line: int

    stack: list[_Chain] = []
    for idx, line in enumerate(lines, start=1):
        stripped = line.lstrip()
        m = re.match(r"#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", stripped)
        if not m:
            if stack:
                stack[-1].body.append(line)
            continue
        kind, rest = m.group(1), m.group(2)
        if kind in ("if", "ifdef", "ifndef"):
            stack.append(
                _Chain([_cond_of_directive(kind, rest)], False, [], idx)
            )
        elif kind == "elif":
            if stack:
                stack[-1].conds.append(rest.strip())
        elif kind == "else":
            if stack:
                stack[-1].has_else = True
        elif kind == "endif":
            if not stack:
                continue
            chain = stack.pop()
            if stack:
                stack[-1].body.extend(chain.body)
            arm = any(_ARM_MACRO.search(c) for c in chain.conds)
            if not arm:
                continue
            body_text = "\n".join(chain.body)
            if not _SIMD_BODY.search(body_text):
                continue
            has_x86 = any(_X86_MACRO.search(c) for c in chain.conds)
            if has_x86 or chain.has_else:
                continue
            findings.append(
                Finding(
                    rel_path,
                    chain.start_line,
                    2,
                    "SIMD code is gated on an ARM macro with no x86_64/SSE "
                    "sibling branch and no #else fallback",
                    lines[chain.start_line - 1],
                )
            )
    return findings


# ── CMake / fetch-script analysis (classes 3, 4, 5) ──────────────────────────
def find_arch_strings(
    rel_path: str, text: str, allow: Allowlist
) -> list[Finding]:
    """Class 3."""
    findings: list[Finding] = []
    for idx, line in enumerate(text.splitlines(), start=1):
        if _is_comment_line(line):
            continue
        if not _ARCH_STRING.search(line):
            continue
        if allow.allows(rel_path, line):
            continue
        findings.append(
            Finding(
                rel_path,
                idx,
                3,
                "hardcoded arm64 asset/arch string; select the arch from the "
                "TARGET (CMAKE_OSX_ARCHITECTURES) or allowlist it with a reason",
                line,
            )
        )
    return findings


def find_system_processor_apple(
    rel_path: str, text: str, allow: Allowlist
) -> list[Finding]:
    """Class 4 — file-scoped: CMAKE_SYSTEM_PROCESSOR read in an Apple-aware file
    that never consults CMAKE_OSX_ARCHITECTURES."""
    if not _APPLE_CONTEXT.search(text):
        return []
    if _OSX_ARCH_ANY.search(text):
        return []
    findings: list[Finding] = []
    for idx, line in enumerate(text.splitlines(), start=1):
        if _is_comment_line(line):
            continue
        if _SYSTEM_PROCESSOR_SET.search(line):
            continue  # defining it (toolchain file), not reading host arch
        if not _SYSTEM_PROCESSOR_READ.search(line):
            continue
        if allow.allows(rel_path, line):
            continue
        findings.append(
            Finding(
                rel_path,
                idx,
                4,
                "CMAKE_SYSTEM_PROCESSOR (HOST arch) drives an Apple decision but "
                "the file never consults CMAKE_OSX_ARCHITECTURES (TARGET arch)",
                line,
            )
        )
    return findings


def find_hardcoded_osx_arch(
    rel_path: str, text: str, allow: Allowlist
) -> list[Finding]:
    """Class 5."""
    findings: list[Finding] = []
    for idx, line in enumerate(text.splitlines(), start=1):
        if _is_comment_line(line):
            continue
        if "x86_64" in line:
            # The line names x86_64 too — universal or a two-slice recipe.
            continue
        flagged = False
        m = _OSX_ARCH_SET.search(line)
        if m:
            tail = m.group(1)
            vm = re.search(r'"([^"]*)"|([^\s)]+)', tail)
            value = (vm.group(1) or vm.group(2) or "") if vm else ""
            if value.strip().strip('"').strip() == "arm64":
                flagged = True
        d = _OSX_ARCH_DFLAG.search(line)
        if d and d.group(1).strip('"') == "arm64":
            flagged = True
        if not flagged:
            continue
        if allow.allows(rel_path, line):
            continue
        findings.append(
            Finding(
                rel_path,
                idx,
                5,
                "CMAKE_OSX_ARCHITECTURES is hardcoded to arm64-only; a universal "
                "or x86_64 target cannot select its slice",
                line,
            )
        )
    return findings


def scan_file(rel_path: str, text: str, allow: Allowlist) -> list[Finding]:
    findings: list[Finding] = []
    if is_core_source(rel_path):
        findings.extend(find_neon_and_arm_simd(rel_path, text))
    if is_cmake_or_fetch(rel_path):
        findings.extend(find_arch_strings(rel_path, text, allow))
    if is_cmake(rel_path):
        findings.extend(find_system_processor_apple(rel_path, text, allow))
        findings.extend(find_hardcoded_osx_arch(rel_path, text, allow))
    return findings


# ── File discovery ───────────────────────────────────────────────────────────
def _repo_root() -> Path:
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            check=True,
            capture_output=True,
            text=True,
        )
        return Path(out.stdout.strip())
    except (subprocess.SubprocessError, FileNotFoundError):
        return Path(__file__).resolve().parents[2]


def _iter_tree_files(root: Path) -> list[str]:
    rels: list[str] = []
    try:
        out = subprocess.run(
            ["git", "ls-files", "core", "tools/cmake", "tools/scripts"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        )
        rels = [ln for ln in out.stdout.splitlines() if ln.strip()]
    except (subprocess.SubprocessError, FileNotFoundError):
        for base in ("core", "tools/cmake", "tools/scripts"):
            for p in (root / base).rglob("*"):
                if p.is_file():
                    rels.append(str(p.relative_to(root)))
    return rels


def _iter_changed_files(root: Path, base: str) -> list[str]:
    try:
        out = subprocess.run(
            ["git", "diff", "--name-only", f"{base}...HEAD"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        )
        names = [ln for ln in out.stdout.splitlines() if ln.strip()]
    except subprocess.SubprocessError:
        # Fall back to the two-dot form (no merge-base) when the base ref has no
        # common ancestor in a shallow checkout.
        out = subprocess.run(
            ["git", "diff", "--name-only", base],
            cwd=root,
            check=False,
            capture_output=True,
            text=True,
        )
        names = [ln for ln in out.stdout.splitlines() if ln.strip()]
    return names


def run(mode: str, base: str, root: Path, allowlist_path: Path) -> list[Finding]:
    allow = Allowlist.load(allowlist_path)
    if mode == "changed":
        rels = _iter_changed_files(root, base)
    else:
        rels = _iter_tree_files(root)

    findings: list[Finding] = []
    for rel in rels:
        if not (is_core_source(rel) or is_cmake_or_fetch(rel) or is_cmake(rel)):
            continue
        fpath = root / rel
        if not fpath.is_file():
            continue  # deleted in `changed` mode
        try:
            text = fpath.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        findings.extend(scan_file(rel, text, allow))
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--mode",
        choices=["changed", "tree"],
        default="tree",
        help="tree: scan every in-scope tracked file (configure-time / nightly). "
        "changed: scan only files changed vs --base (fast PR path).",
    )
    parser.add_argument(
        "--base",
        default=os.environ.get("PULP_INTEL_CANARY_BASE", "origin/main"),
        help="Base ref for --mode=changed (default origin/main).",
    )
    parser.add_argument("--root", default=None, help="Repo root (default: git).")
    parser.add_argument(
        "--allowlist",
        default=None,
        help="Allowlist file (default: tools/scripts/intel_canary_allowlist.txt).",
    )
    args = parser.parse_args(argv)

    root = Path(args.root).resolve() if args.root else _repo_root()
    allowlist_path = (
        Path(args.allowlist)
        if args.allowlist
        else root / "tools" / "scripts" / "intel_canary_allowlist.txt"
    )

    findings = run(args.mode, args.base, root, allowlist_path)
    if not findings:
        print(
            f"intel-canary: clean ({args.mode} mode) — no cross-arch "
            "portability regressions found."
        )
        return 0

    findings.sort(key=lambda f: (f.klass, f.path, f.line))
    print(
        f"intel-canary: {len(findings)} cross-arch portability finding(s) "
        f"({args.mode} mode):\n",
        file=sys.stderr,
    )
    seen_classes = sorted({f.klass for f in findings})
    for f in findings:
        print(f.render(), file=sys.stderr)
    print("", file=sys.stderr)
    for k in seen_classes:
        print(f"  C{k}: {CLASS_TITLES[k]}", file=sys.stderr)
    print(
        "\nThese are compile-time-on-Intel or build-system arch regressions. "
        "Fix them at the source; do not weaken the lint. If an occurrence is "
        "genuinely arch-aware, record it with a reason in "
        "tools/scripts/intel_canary_allowlist.txt. "
        "See docs/guides/intel-support.md.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
