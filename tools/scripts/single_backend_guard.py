#!/usr/bin/env python3
"""Static governance guard for the SignalGraph single-backend invariants.

Pulp runs *one* inter-node routing backend (`GraphRuntimeExecutor`) under *one*
authoring unit (`Processor`), and generated DSP reaches a graph only through the
two sanctioned ABIs. This guard turns the SignalGraph / linear-authoring
contract's structural invariants into enforced assertions so convergence work
cannot silently regrow a second runtime, a second authoring surface, or an
unsanctioned generated-DSP entrypoint.

It is a *static* guard — file and symbol presence/allowlist — deliberately
low-false-positive and review-gated. Each allowlist below names the surface that
is sanctioned today; growing one is an intentional act, visible in the diff and
reviewable. The deeper *behavioral* guarantee (the routed backend stays
bit-identical to the SignalGraph walk) is the differential parity test this guard
keeps wired into the build, not this static scan.

Invariants enforced:

  1. Single inter-node routing backend (I10/I11). The routed-graph execution
     entry points (`process_routed` / `process_parallel`) may be *declared or
     defined as a member* only in the one sanctioned translation unit
     (`graph_runtime_executor.{hpp,cpp}`). Any other file that defines such a
     member — inline in a class, or out-of-line — is a second routing engine.
     Call sites (`obj.process_routed(...)`, `ptr->process_parallel(...)`) and
     references to `GraphRuntimeExecutor::process_routed` are fine anywhere.

  2. No second authoring surface (I1). `pulp create` scaffolds default to a
     `Processor`; no builtin template offers a graph as the authoring surface.

  3. No unsanctioned generated-DSP entrypoint (I12). The public generated-DSP
     ABI surface under `core/native-components/include` is frozen: the header
     files are the sanctioned set, and their exported entry symbols are the
     sanctioned pair — `pulp_native_core_entry_v1` (the generated *plugin* DSP
     path → `NativeCoreProcessor` → `ProcessorNode`) and `pulp_node_v1_entry`
     (the constrained custom-`SignalGraph`-node ABI). A new public ABI header or
     entry symbol is a new generated-DSP authoring surface and needs
     architecture review — recorded here as a deliberate allowlist edit.

  4. Parity safety-net present (required check). The randomized differential
     parity test source exists and is registered (in a non-comment line) as a
     built test target, so the ctest suite keeps running the routed-vs-walk
     equivalence proof. This is what makes the parity matrix a required check
     rather than a deletable one.

Usage:
    python3 tools/scripts/single_backend_guard.py [--root DIR]
    python3 tools/scripts/single_backend_guard.py --selftest

Exits 0 when every assertion holds, 1 on any violation (or a failing self-test).
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

# ── Sanctioned surfaces (the governance line) ──────────────────────────────
# Each set names what is sanctioned *today*. Adding an entry is an intentional,
# reviewable act — it widens what the codebase is allowed to contain.

# The one type permitted to own routed-graph execution entry points, and the one
# translation unit those members may be declared/defined in.
SANCTIONED_ROUTED_EXECUTORS = frozenset({"GraphRuntimeExecutor"})
SANCTIONED_EXECUTOR_FILES = frozenset(
    {"graph_runtime_executor.hpp", "graph_runtime_executor.cpp"}
)

# The public generated-DSP ABI surface: the header files, and the entry symbols
# they export. Two ABIs exist by design and do not wrap each other (see
# native_core.h / pulp_node_v1.h): the Processor-level FFI and the constrained
# custom-node ABI. A third header or entry symbol is a new authoring surface.
SANCTIONED_NATIVE_ABI_HEADERS = frozenset(
    {"native_core.h", "native_core.hpp", "pulp_node_v1.h", "pulp_node_v1.hpp"}
)
SANCTIONED_GENERATED_DSP_ENTRYPOINTS = frozenset(
    {"pulp_native_core_entry_v1", "pulp_node_v1_entry"}
)

# ── Static signals ─────────────────────────────────────────────────────────

ROUTED_METHODS = ("process_routed", "process_parallel")

# An occurrence of a routed-execution entry point as a name followed by `(`.
ROUTED_OCCURRENCE_RE = re.compile(r"\b(process_routed|process_parallel)\s*\(")

# The qualifier of an out-of-line `Qualifier::method(` occurrence.
QUALIFIER_RE = re.compile(r"([A-Za-z_]\w*)\s*::\s*$")

# A public C-ABI generated-DSP entry symbol: `pulp_..._entry...(void)` or `(`.
# Lowercase `pulp_` avoids the uppercase `PULP_NODE_V1_EXPORT` storage macro; the
# `entry` anchor and `(...)` avoid the `pulp_node_entry_v1` struct typedef.
ABI_ENTRY_RE = re.compile(r"\b(pulp_[a-z0-9_]*entry[a-z0-9_]*)\s*\(\s*(?:void)?\s*\)")

# A builtin `pulp create` template token: `template_arg == "name"`.
CREATE_TEMPLATE_RE = re.compile(r'template_arg\s*==\s*"([^"]+)"')

# A create template / template dir that offers a graph as the authoring surface.
# Word-boundary anchored so unrelated names like "graphite"/"graphics" are safe,
# while a bare "graph", "signal-graph", or "graph-plugin" token is forbidden.
FORBIDDEN_TEMPLATE_TOKEN_RE = re.compile(
    r"\b(graph|signal[\s_-]?graph|graph[\s_-]?plugin)\b", re.IGNORECASE
)

SOURCE_SCAN_SUFFIXES = (".cpp", ".cc", ".mm", ".hpp", ".h")
ABI_SCAN_SUFFIXES = (".h", ".hpp")

PARITY_TEST_SOURCE = "test/test_graph_routing_differential_parity.cpp"
CREATE_CMD_SOURCE = "tools/cli/cmd_create.cpp"
NATIVE_ABI_INCLUDE_DIR = "core/native-components/include"


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return ""


def _strip_cpp_comments(text: str) -> list[str]:
    """Blank out // and /* */ comments, preserving line count and same-line
    column structure (so "token immediately before X" logic stays valid)."""
    out: list[str] = []
    in_block = False
    for line in text.splitlines():
        buf = []
        i, n = 0, len(line)
        while i < n:
            if in_block:
                end = line.find("*/", i)
                if end == -1:
                    buf.append(" " * (n - i))
                    i = n
                else:
                    buf.append(" " * (end + 2 - i))
                    i = end + 2
                    in_block = False
            elif line.startswith("//", i):
                buf.append(" " * (n - i))
                i = n
            elif line.startswith("/*", i):
                in_block = True
                buf.append("  ")
                i += 2
            else:
                buf.append(line[i])
                i += 1
        out.append("".join(buf))
    return out


def _strip_cmake_comments(text: str) -> str:
    """Drop `#` comments from CMake text (no `#` inside the paths we check)."""
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def check_single_backend(root: Path) -> list[str]:
    """I10/I11 — routed-execution members live only in the sanctioned TU."""
    violations: list[str] = []
    core = root / "core"
    if not core.is_dir():
        return [f"single-backend: expected source tree {core} is missing"]

    found_sanctioned = False
    for f in sorted(core.rglob("*")):
        if not (f.is_file() and f.suffix in SOURCE_SCAN_SUFFIXES):
            continue
        in_executor_tu = f.name in SANCTIONED_EXECUTOR_FILES
        for lineno, line in enumerate(_strip_cpp_comments(_read(f)), start=1):
            for m in ROUTED_OCCURRENCE_RE.finditer(line):
                before = line[: m.start()].rstrip()
                if before.endswith(".") or before.endswith("->"):
                    continue  # method call — fine anywhere
                if before.endswith("::"):
                    qm = QUALIFIER_RE.search(before)
                    qualifier = qm.group(1) if qm else ""
                    if qualifier in SANCTIONED_ROUTED_EXECUTORS:
                        found_sanctioned = True
                        continue
                    # A non-sanctioned `Rival::process_routed(` is a second routing
                    # engine WHEREVER it is defined — including inside the sanctioned
                    # executor TU (a second engine must not hide in the same file).
                    rel = f.relative_to(root)
                    violations.append(
                        f"{rel}:{lineno}: a second routing engine — "
                        f"`{qualifier}::{m.group(1)}` defines a routed-graph "
                        f"execution entry point. Only "
                        f"{sorted(SANCTIONED_ROUTED_EXECUTORS)} may (one backend, "
                        f"one semantics). Route through GraphRuntimeExecutor."
                    )
                    continue
                # Bare occurrence: an inline member decl/def, or an out-of-line def
                # whose `Qualifier::` sits on the previous line. Allowed only inside
                # the sanctioned executor TU — where it is GraphRuntimeExecutor's own
                # in-class declaration. (Residual blind spot: a rival class defined
                # *inline* inside graph_runtime_executor.{hpp,cpp} with a bare
                # process_routed member would pass; the realistic out-of-line
                # `Rival::process_routed` form is caught above, in any file.)
                if in_executor_tu:
                    found_sanctioned = True
                    continue
                rel = f.relative_to(root)
                violations.append(
                    f"{rel}:{lineno}: a second routing engine — `{m.group(1)}` is "
                    f"declared/defined as a member outside "
                    f"graph_runtime_executor.{{hpp,cpp}}. The routed-graph "
                    f"execution entry points belong to one backend. Route through "
                    f"GraphRuntimeExecutor instead."
                )

    if not found_sanctioned:
        violations.append(
            "single-backend: no GraphRuntimeExecutor routed-execution member found "
            "(process_routed/process_parallel) — the sanctioned routing backend "
            "appears to be gone."
        )
    return violations


def check_authoring_surface(root: Path) -> list[str]:
    """I1 — scaffolds default to a Processor; no graph authoring template."""
    violations: list[str] = []
    create_src = root / CREATE_CMD_SOURCE
    if not create_src.is_file():
        return [
            f"authoring-surface: {CREATE_CMD_SOURCE} is missing — cannot verify "
            "scaffolds default to a Processor."
        ]

    text = _read(create_src)
    tokens = CREATE_TEMPLATE_RE.findall(text)
    if not tokens:
        violations.append(
            f"authoring-surface: no builtin `template_arg == \"...\"` tokens found in "
            f"{CREATE_CMD_SOURCE} — the create scaffold allowlist may have moved; "
            "update this guard's signal."
        )
    for tok in tokens:
        if FORBIDDEN_TEMPLATE_TOKEN_RE.search(tok):
            violations.append(
                f"{CREATE_CMD_SOURCE}: builtin template '{tok}' offers a graph "
                "authoring surface. A plugin is a Processor; the SignalGraph DAG is "
                "an optional composition layer, never a scaffolded authoring default."
            )

    templates_dir = root / "templates"
    if templates_dir.is_dir():
        for child in sorted(templates_dir.iterdir()):
            if child.is_dir() and FORBIDDEN_TEMPLATE_TOKEN_RE.search(child.name):
                violations.append(
                    f"templates/{child.name}: a graph-authoring scaffold template is "
                    "a second authoring surface — remove it; scaffolds default to a "
                    "Processor."
                )
    return violations


def check_generated_dsp_entrypoints(root: Path) -> list[str]:
    """I12 — the public generated-DSP ABI surface stays the sanctioned set."""
    violations: list[str] = []
    include_dir = root / NATIVE_ABI_INCLUDE_DIR
    if not include_dir.is_dir():
        return [
            f"generated-dsp: {NATIVE_ABI_INCLUDE_DIR} is missing — cannot verify the "
            "generated-DSP ABI surface."
        ]

    found: dict[str, str] = {}
    for f in sorted(include_dir.rglob("*")):
        if not (f.is_file() and f.suffix in ABI_SCAN_SUFFIXES):
            continue
        rel = f.relative_to(root)
        if f.name not in SANCTIONED_NATIVE_ABI_HEADERS:
            violations.append(
                f"{rel}: unsanctioned public generated-DSP ABI header. A new ABI "
                "surface under this include tree needs architecture review (I12). If "
                "intentional and reviewed, add it to SANCTIONED_NATIVE_ABI_HEADERS "
                "in this guard."
            )
        for lineno, line in enumerate(_strip_cpp_comments(_read(f)), start=1):
            for m in ABI_ENTRY_RE.finditer(line):
                found.setdefault(m.group(1), f"{rel}:{lineno}")

    for symbol, loc in sorted(found.items()):
        if symbol not in SANCTIONED_GENERATED_DSP_ENTRYPOINTS:
            violations.append(
                f"{loc}: unsanctioned generated-DSP ABI entrypoint `{symbol}`. A new "
                "public generated-DSP ABI needs architecture review (I12). Generated "
                "plugin DSP uses native_core.h → NativeCoreProcessor → ProcessorNode; "
                "custom nodes use pulp_node_v1. If this entrypoint is intentional and "
                "reviewed, add it to SANCTIONED_GENERATED_DSP_ENTRYPOINTS in this guard."
            )

    missing = SANCTIONED_GENERATED_DSP_ENTRYPOINTS - set(found)
    if missing:
        violations.append(
            "generated-dsp: sanctioned ABI entrypoint(s) "
            f"{sorted(missing)} not found under {NATIVE_ABI_INCLUDE_DIR} — the "
            "generated-DSP path appears to have changed shape; update this guard."
        )
    return violations


def check_parity_safety_net(root: Path) -> list[str]:
    """Required check — the differential parity test stays wired into the build."""
    violations: list[str] = []
    parity_src = root / PARITY_TEST_SOURCE
    if not parity_src.is_file():
        return [
            f"parity-safety-net: {PARITY_TEST_SOURCE} is missing — the routed-vs-walk "
            "differential parity proof must exist and stay in the suite."
        ]

    basename = Path(PARITY_TEST_SOURCE).name
    build_files = [root / "test" / "CMakeLists.txt"]
    cmake_dir = root / "test" / "cmake"
    if cmake_dir.is_dir():
        build_files.extend(sorted(cmake_dir.glob("*.cmake")))

    # The basename must appear on a real build-registration line, not merely
    # anywhere in the file — a dead `set(X test_..._parity.cpp)` or a mention in
    # an unrelated target would otherwise satisfy a plain substring search while
    # ctest never builds the test. Require it to share a non-comment line with a
    # registration keyword.
    REGISTRATION_MARKERS = ("pulp_add_test_suite", "add_executable", "target_sources", "SOURCES")
    registered = False
    for bf in build_files:
        if not bf.is_file():
            continue
        for line in _strip_cmake_comments(_read(bf)).splitlines():
            if basename in line and any(mk in line for mk in REGISTRATION_MARKERS):
                registered = True
                break
        if registered:
            break
    if not registered:
        violations.append(
            f"parity-safety-net: {PARITY_TEST_SOURCE} exists but is not registered as a "
            "built test (no SOURCES/add_executable/target_sources/pulp_add_test_suite line "
            "names it) in test/CMakeLists.txt or test/cmake/*.cmake — an unregistered test "
            "is never built or run by ctest, so the parity check would silently stop gating."
        )
    return violations


CHECKS = (
    check_single_backend,
    check_authoring_surface,
    check_generated_dsp_entrypoints,
    check_parity_safety_net,
)


def run(root: Path) -> list[str]:
    violations: list[str] = []
    for check in CHECKS:
        violations.extend(check(root))
    return violations


# ── Self-test ──────────────────────────────────────────────────────────────


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _make_clean_fixture(root: Path) -> None:
    """A minimal repo layout that satisfies every assertion, including the
    call-site / comment / qualifier-reference shapes that must NOT trip."""
    _write(
        root / "core/format/src/graph_runtime_executor.cpp",
        "GraphRuntimeExecutorResult GraphRuntimeExecutor::process_routed(int x) {}\n"
        "GraphRuntimeExecutorResult GraphRuntimeExecutor::process_parallel(int x) {}\n",
    )
    # Calls, comments, and qualifier references elsewhere must stay clean.
    _write(
        root / "core/host/src/signal_graph.cpp",
        "void run(GraphRuntimeExecutor& e) {\n"
        "  // process_routed( in a comment must not count\n"
        "  e.process_routed(0);\n"
        "  auto* p = &e; p->process_parallel(0);\n"
        "  /* GraphRuntimeExecutor::process_routed( in a block comment */\n"
        "}\n",
    )
    _write(
        root / "tools/cli/cmd_create.cpp",
        'bool ok() { return template_arg == "effect" || template_arg == "instrument"\n'
        '    || template_arg == "app" || template_arg == "bare"; }\n',
    )
    _write(
        root / f"{NATIVE_ABI_INCLUDE_DIR}/pulp/native_components/native_core.h",
        "const pulp_native_core_v1* pulp_native_core_entry_v1(void);\n",
    )
    _write(
        root / f"{NATIVE_ABI_INCLUDE_DIR}/pulp/native_components/pulp_node_v1.h",
        "const pulp_node_entry_v1* pulp_node_v1_entry(void);\n"
        "typedef struct pulp_node_entry_v1 { int x; } pulp_node_entry_v1;\n",
    )
    _write(root / PARITY_TEST_SOURCE, 'TEST_CASE("parity") {}\n')
    _write(
        root / "test/cmake/sampler_runtime_tests.cmake",
        "pulp_add_test_suite(pulp-test-graph-routing-differential-parity\n"
        "    SOURCES test_graph_routing_differential_parity.cpp)\n",
    )


def selftest() -> int:
    failures = 0

    def case(name: str, mutate, expect_substr) -> None:
        nonlocal failures
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _make_clean_fixture(root)
            if mutate:
                mutate(root)
            found = run(root)
            if expect_substr is None:
                ok = not found
            else:
                ok = any(expect_substr in v for v in found)
            status = "ok" if ok else "FAIL"
            if not ok:
                failures += 1
            detail = "clean" if expect_substr is None else f"expects {expect_substr!r}"
            print(f"[{status}] {name} ({detail}); found={len(found)}")
            if not ok and found:
                for v in found:
                    print(f"        · {v}")

    case("clean tree passes (calls/comments/qualifier refs)", None, None)

    def add_oneline_engine(root: Path) -> None:
        _write(
            root / "core/format/src/rival_executor.cpp",
            "Result RivalExecutor::process_routed(int x) {}\n",
        )

    case("out-of-line second engine fails", add_oneline_engine, "second routing engine")

    def add_inline_engine(root: Path) -> None:
        _write(
            root / "core/format/include/rival.hpp",
            "struct RivalExecutor {\n  Result process_routed(int x) { return {}; }\n};\n",
        )

    case("inline-in-header second engine fails", add_inline_engine, "second routing engine")

    def add_multiline_engine(root: Path) -> None:
        _write(
            root / "core/format/src/rival2.cpp",
            "Result RivalExecutor::\nprocess_parallel(int x) {}\n",
        )

    case("multiline second engine fails", add_multiline_engine, "second routing engine")

    def add_rival_in_executor_tu(root: Path) -> None:
        # A second engine defined out-of-line INSIDE the sanctioned executor file
        # must not escape via the TU filename.
        p = root / "core/format/src/graph_runtime_executor.cpp"
        p.write_text(p.read_text() + "Result RivalExecutor::process_routed(int x) {}\n")

    case("second engine hiding in the executor TU fails", add_rival_in_executor_tu, "second routing engine")

    def delete_backend(root: Path) -> None:
        (root / "core/format/src/graph_runtime_executor.cpp").unlink()

    case("missing backend fails", delete_backend, "sanctioned routing backend")

    def add_graph_template(root: Path) -> None:
        p = root / "tools/cli/cmd_create.cpp"
        p.write_text(
            p.read_text() + '\nbool g() { return template_arg == "graph-plugin"; }\n'
        )

    case("graph create template fails", add_graph_template, "graph authoring surface")

    def add_graphite_template(root: Path) -> None:
        p = root / "tools/cli/cmd_create.cpp"
        p.write_text(
            p.read_text() + '\nbool g() { return template_arg == "graphite-ui"; }\n'
        )

    case("graphite template is NOT flagged (word boundary)", add_graphite_template, None)

    def add_graph_template_dir(root: Path) -> None:
        (root / "templates/signal-graph").mkdir(parents=True)

    case("graph template dir fails", add_graph_template_dir, "second authoring surface")

    def add_new_abi_symbol(root: Path) -> None:
        # New entry symbol inside an *existing* sanctioned header.
        p = root / f"{NATIVE_ABI_INCLUDE_DIR}/pulp/native_components/native_core.h"
        p.write_text(p.read_text() + "const void* pulp_ai_dsp_entry_v2(void);\n")

    case("new ABI entry symbol fails", add_new_abi_symbol, "unsanctioned generated-DSP ABI entrypoint")

    def add_new_abi_header(root: Path) -> None:
        # New ABI header that avoids the `_entry` naming convention entirely.
        _write(
            root / f"{NATIVE_ABI_INCLUDE_DIR}/pulp/native_components/pulp_secret_abi.h",
            "const void* pulp_secret_init(void);\n",
        )

    case("new ABI header fails (even without _entry name)", add_new_abi_header, "unsanctioned public generated-DSP ABI header")

    def drop_one_sanctioned_symbol(root: Path) -> None:
        p = root / f"{NATIVE_ABI_INCLUDE_DIR}/pulp/native_components/native_core.h"
        p.write_text("/* entry symbol removed */\n")

    case("missing sanctioned ABI symbol fails", drop_one_sanctioned_symbol, "not found under")

    def comment_only_parity(root: Path) -> None:
        (root / "test/cmake/sampler_runtime_tests.cmake").write_text(
            "pulp_add_test_suite(pulp-test-other SOURCES test_other.cpp)\n"
            "# SOURCES test_graph_routing_differential_parity.cpp (disabled)\n"
        )

    case("comment-only parity registration fails", comment_only_parity, "not registered")

    def dead_var_parity_mention(root: Path) -> None:
        # The basename appears, but only in a dead variable — not a real test
        # registration, so ctest never builds it.
        (root / "test/cmake/sampler_runtime_tests.cmake").write_text(
            "pulp_add_test_suite(pulp-test-other SOURCES test_other.cpp)\n"
            "set(UNUSED_SRC test_graph_routing_differential_parity.cpp)\n"
        )

    case("dead-variable parity mention fails", dead_var_parity_mention, "not registered")

    def drop_parity_registration(root: Path) -> None:
        (root / "test/cmake/sampler_runtime_tests.cmake").write_text(
            "pulp_add_test_suite(pulp-test-other SOURCES test_other.cpp)\n"
        )

    case("unregistered parity test fails", drop_parity_registration, "not registered")

    def delete_parity_source(root: Path) -> None:
        (root / PARITY_TEST_SOURCE).unlink()

    case("missing parity test fails", delete_parity_source, "is missing")

    if failures:
        print(f"\nselftest: {failures} case(s) failed")
        return 1
    print("\nselftest: all cases passed")
    return 0


def main(argv) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=None, help="repo root (default: auto-detect)")
    parser.add_argument("--selftest", action="store_true", help="run fixture self-test")
    args = parser.parse_args(argv[1:])

    if args.selftest:
        return selftest()

    root = Path(args.root) if args.root else Path(__file__).resolve().parents[2]
    violations = run(root)
    if violations:
        for v in violations:
            print(v)
        print(
            f"\n{len(violations)} single-backend governance violation(s) found.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
