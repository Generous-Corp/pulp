#!/usr/bin/env python3
"""RT-02 source contract for the prepared sample-region executor.

This is deliberately a source-anchored contract, rather than a mirror test:
the function body is located by its C++ signature and balanced braces, then
checked for the operations that are forbidden on the process-time path.  The
contract complements the runtime allocation/lock probe used by the normal
sample-region test suite.  It does not copy the executor's algorithm or
assert an implementation-specific operation count.

Usage:
    python3 test/sample_region_i2/rt02_process_contract.py --repo-root .
    python3 test/sample_region_i2/rt02_process_contract.py --repo-root . \
        --negative-control
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


SOURCE = Path("core/host/src/sample_region_runtime.cpp")
HEADER = Path("core/host/include/pulp/host/sample_region_runtime.hpp")
SIGNATURE = "void PreparedSampleRegion::process("

# These are process-time side effects or discovery mechanisms.  The executor
# is allowed to dereference already-prepared descriptor callbacks; it must not
# rediscover a type, walk a graph, perform I/O, or introduce an exception path.
FORBIDDEN = {
    "logging": (
        r"\bruntime::log_[A-Za-z_]+\b",
        r"\b(?:std::)?(?:cout|cerr|clog|cin)\b",
        r"\b(?:printf|fprintf|fwrite|fopen|open|read|write|close)\s*\(",
        r"\bstd::(?:i|o|io)fstream\b",
        r"\b(?:iostream|unistd\.h)\b",
    ),
    "exceptions": (
        r"\b(?:try|catch|throw)\b",
        r"\bstd::exception\b",
        r"\b(?:make_exception|current_exception)\b",
    ),
    "type lookup": (
        r"\b(?:sample_kernel_type|type_id|lookup|registry|unordered_map|std::map)\b",
        r"\.(?:find|at)\s*\(",
    ),
    "graph walk": (
        r"\b(?:SignalGraph|graph_|NodeId|connect|topolog(?:y|ical)|walk|edges?|nodes?)\b",
    ),
    "process allocation": (
        r"\b(?:new|delete|malloc|calloc|realloc|free)\b",
        r"\b(?:make_shared|make_unique)\b",
    ),
}

# A process block may scale with prepared storage and the host-provided frame
# count.  Any other loop shape is a review point: in particular, while/do loops
# and bounds derived from sample values make data-dependent work easy to hide.
ALLOWED_FOR_HEADERS = (
    r"promoted_values_\.size\(\)",
    r"num_samples",
    r"input_boundaries_",
    r"output_boundaries_",
    r"output\.num_channels\(\)",
    r"plan_\.operations",
    r"scalar_slots_\.size\(\)",
)


def _strip_comments_and_literals(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.S)
    source = re.sub(r"//[^\n]*", " ", source)
    source = re.sub(r'"(?:\\.|[^"\\])*"', '""', source)
    source = re.sub(r"'(?:\\.|[^'\\])*'", "''", source)
    return source


def _extract_function(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise ValueError(f"missing process signature: {signature}")
    brace = source.find("{", start)
    if brace < 0:
        raise ValueError("process signature has no body")
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise ValueError("unterminated process body")


def _loop_headers(body: str) -> list[str]:
    # The process implementation uses range-for and ordinary for loops.  This
    # intentionally does not parse all C++; it only identifies loop headers so
    # an added while/do or a new data-derived bound cannot pass silently.
    headers = re.findall(r"\bfor\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)", body)
    return [" ".join(header.split()) for header in headers]


def verify_text(header_text: str, source_text: str) -> list[str]:
    errors: list[str] = []
    normalized_header = _strip_comments_and_literals(header_text)
    normalized_source = _strip_comments_and_literals(source_text)

    declaration = re.search(
        r"void\s+process\s*\([^;{}]*\)\s*noexcept\s*;", normalized_header, re.S
    )
    if declaration is None:
        errors.append("PreparedSampleRegion::process declaration must be noexcept")

    try:
        body = _extract_function(normalized_source, SIGNATURE)
    except ValueError as exc:
        return [str(exc)]

    signature_start = normalized_source.find(SIGNATURE)
    signature_end = normalized_source.find("{", signature_start)
    if "noexcept" not in normalized_source[signature_start:signature_end]:
        errors.append("PreparedSampleRegion::process definition must be noexcept")

    for category, patterns in FORBIDDEN.items():
        for pattern in patterns:
            if re.search(pattern, body):
                errors.append(f"forbidden {category} token: {pattern}")

    if re.search(r"\b(?:while|do)\b", body):
        errors.append("process body may not contain while/do loops")

    headers = _loop_headers(body)
    if not headers:
        errors.append("process body must expose its bounded prepared/frame loops")
    for header in headers:
        if not any(re.search(allowed, header) for allowed in ALLOWED_FOR_HEADERS):
            errors.append(f"loop bound is not prepared/frame bounded: {header}")

    # Calls through prepared descriptor callbacks are the only indirect work
    # permitted here.  This catches an accidental call back into graph/runtime
    # discovery without constraining callback implementations themselves.
    for call in re.findall(r"\b([A-Za-z_][A-Za-z0-9_.]*)\s*\(", body):
        if call.split(".")[-1] in {"if", "for", "switch", "while"}:
            continue
        if call in {
            "std::fill",
            "input.num_channels",
            "input.num_samples",
            "output.num_channels",
            "output.num_samples",
            "input.channel_ptr",
            "output.channel_ptr",
            "parameters_->value",
            "static_cast",
        }:
            continue
        if call.endswith(".size") or call.endswith(".data"):
            continue
        if call.startswith("kernel.descriptor."):
            continue
        # A call can be a C++ cast or a control expression not captured above;
        # only flag named runtime/discovery surfaces, leaving ordinary syntax
        # to the token checks above.
        if re.search(r"(?:graph|lookup|find|type|log|io|file|throw)", call, re.I):
            errors.append(f"process call reaches runtime/discovery surface: {call}")

    return errors


def verify(repo_root: Path) -> list[str]:
    source_path = repo_root / SOURCE
    header_path = repo_root / HEADER
    if not source_path.is_file():
        return [f"missing executor source: {source_path}"]
    if not header_path.is_file():
        return [f"missing executor header: {header_path}"]
    return verify_text(header_path.read_text(encoding="utf-8"),
                      source_path.read_text(encoding="utf-8"))


def negative_control(repo_root: Path) -> list[str]:
    source_path = repo_root / SOURCE
    header_path = repo_root / HEADER
    source = source_path.read_text(encoding="utf-8")
    header = header_path.read_text(encoding="utf-8")
    def mutate_once(needle: str, replacement: str) -> str:
        return source.replace(needle, replacement, 1)

    mutations = {
        "logging": mutate_once(
            "if (num_samples <= 0",
            'runtime::log_info("bad");\n    if (num_samples <= 0',
        ),
        "exception": mutate_once(
            "if (num_samples <= 0", "try { if (num_samples <= 0"
        ),
        "type-lookup": mutate_once(
            "for (const auto& operation",
            "plan_.kernels.find(0);\n        for (const auto& operation",
        ),
        "graph-walk": mutate_once(
            "for (const auto& operation",
            "SignalGraph graph;\n        for (const auto& operation",
        ),
        "unbounded-loop": mutate_once(
            "for (int frame_index",
            "while (num_samples-- > 0) { for (int frame_index",
        ),
    }
    failures: list[str] = []
    for name, mutated in mutations.items():
        if mutated == source:
            failures.append(f"negative control mutation did not apply: {name}")
            continue
        if verify_text(header, mutated):
            print(f"rt02_process_contract_case={name}")
        else:
            failures.append(f"negative control escaped: {name}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()

    errors = verify(args.repo_root)
    if errors:
        for error in errors:
            print(f"error={error}", file=sys.stderr)
        return 1
    print("rt02_process_contract_case=valid-current-executor")

    if args.negative_control:
        errors = negative_control(args.repo_root)
        if errors:
            for error in errors:
                print(f"error={error}", file=sys.stderr)
            return 1
    print("rt02_process_contract_verified=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
