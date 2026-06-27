#!/usr/bin/env python3
"""Report test-suite quality signals without changing test behavior.

This is Phase 0 infrastructure for the test-suite quality remediation plan:
make the current signal visible before flipping any enforcement gates.

The report combines:
- CTest registration data from `ctest --show-only=json-v1`.
- Inline CTest exclude regex fragments from the main build workflow.
- Weak-test anti-pattern counts in test-like source files.

Run from the repo root:
    python3 tools/scripts/test_suite_quality_report.py --format markdown
"""

from __future__ import annotations

import argparse
import collections
import datetime as _dt
import fnmatch
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


TIER_LABELS = {
    "unit",
    "integration",
    "regression",
    "fuzz",
    "golden",
    "performance",
    "rt-safety",
    "platform",
    "smoke",
    "visual",
    "validator",
}
QUARANTINE_PATH = Path("tools/test-quality/quarantine.json")
WEAK_PATTERN_BASELINE_PATH = Path("tools/test-quality/weak-pattern-baseline.json")
WEAK_PATTERN_EXEMPTIONS_PATH = Path("tools/test-quality/weak-pattern-exemptions.json")
CI_TIERS_PATH = Path("tools/test-quality/ci-tiers.json")
MODULE_COVERAGE_PATH = Path("tools/test-quality/module-coverage.json")
MODULE_COVERAGE_UNMAPPED_TRIAGE_PATH = Path(
    "tools/test-quality/module-coverage-unmapped-triage.json"
)
CAPABILITY_LABELS_PATH = Path("tools/test-quality/capabilities.json")
QUARANTINE_REQUIRED_FIELDS = {
    "test_pattern",
    "platform",
    "tier",
    "owner",
    "reason",
    "tracking_issue",
    "expires",
    "max_count_impact",
}
CAPABILITY_REQUIRED_FIELDS = {
    "id",
    "title",
    "owner",
    "description",
    "expected_behavior",
    "command_patterns",
}
MODULE_COVERAGE_UNMAPPED_TRIAGE_REQUIRED_FIELDS = {
    "command",
    "expected_count",
    "owner",
    "reason",
    "next_action",
}
WEAK_PATTERN_EXEMPTION_REQUIRED_FIELDS = {
    "path",
    "kind",
    "classification",
    "text_regex",
    "owner",
    "reason",
    "max_count_impact",
}

TEST_ROOTS = ("test", "tools", "packages", "apple")
TEST_NAME_RE = re.compile(r"(test|Test|spec|Spec)")
WEAK_PATTERNS = {
    "succeed": re.compile(r"\bSUCCEED\s*\("),
    "skip": re.compile(r"\bSKIP\s*\("),
    "require_true": re.compile(r"\bREQUIRE\s*\(\s*true\s*\)"),
}


@dataclass(frozen=True)
class PatternHit:
    kind: str
    path: str
    line: int
    text: str
    classification: str


def repo_root_from(start: Path) -> Path:
    current = start.resolve()
    for candidate in (current, *current.parents):
        if (candidate / ".git").exists() and (candidate / "CMakeLists.txt").exists():
            return candidate
    raise SystemExit(f"could not find repo root from {start}")


def labels_for(test: dict[str, Any]) -> list[str]:
    labels: list[str] = []
    for prop in test.get("properties", []):
        if prop.get("name") != "LABELS":
            continue
        value = prop.get("value")
        if isinstance(value, list):
            labels.extend(str(item) for item in value)
        elif isinstance(value, str):
            labels.extend(part for part in value.split(";") if part)
    return labels


def command_name(test: dict[str, Any]) -> str:
    command = test.get("command")
    if isinstance(command, list) and command:
        return Path(str(command[0])).name
    if isinstance(command, str) and command:
        return Path(command.split()[0]).name
    return "<unknown>"


def load_ctest_json(path: Path | None, build_dir: Path | None) -> dict[str, Any] | None:
    if path is not None:
        return json.loads(path.read_text(encoding="utf-8"))
    if build_dir is None:
        return None
    proc = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise SystemExit(
            "ctest --show-only=json-v1 failed with "
            f"exit {proc.returncode}:\n{proc.stderr}"
        )
    return json.loads(proc.stdout)


def count_numbered_log(path: Path) -> int:
    if not path.exists():
        return 0
    count = 0
    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        if re.match(r"^\d+:", line):
            count += 1
    return count


def parse_last_test_log(path: Path | None) -> dict[str, Any]:
    if path is None or not path.exists():
        return {
            "available": False,
            "selected": 0,
            "executed": 0,
            "passed": 0,
            "failed": 0,
            "skipped": 0,
        }

    text = path.read_text(encoding="utf-8", errors="ignore")
    selected = 0
    for match in re.finditer(r"(?m)^\d+/(\d+)\s+Testing:", text):
        selected = max(selected, int(match.group(1)))

    passed = len(re.findall(r"(?m)^Test Passed\.", text))
    failed = len(re.findall(r"(?m)^Test Failed\.", text))
    skipped = len(re.findall(r"(?m)^Test Skipped\.", text))
    # Some CTest versions write "Test Not Run." for disabled/skipped cases.
    skipped += len(re.findall(r"(?m)^Test Not Run\.", text))
    executed = passed + failed + skipped

    return {
        "available": True,
        "selected": selected,
        "executed": executed,
        "passed": passed,
        "failed": failed,
        "skipped": skipped,
    }


def summarize_ctest_execution(build_dir: Path | None) -> dict[str, Any]:
    if build_dir is None:
        result = parse_last_test_log(None)
        result.update({
            "disabled": 0,
            "failed_log_entries": 0,
        })
        return result

    temporary = build_dir / "Testing" / "Temporary"
    result = parse_last_test_log(temporary / "LastTest.log")
    result.update({
        "disabled": count_numbered_log(temporary / "LastTestsDisabled.log"),
        "failed_log_entries": count_numbered_log(temporary / "LastTestsFailed.log"),
    })
    return result


def summarize_ctest(doc: dict[str, Any] | None) -> dict[str, Any]:
    if doc is None:
        return {
            "available": False,
            "registered": 0,
            "label_counts": {},
            "tier_label_counts": {},
            "unlabelled": 0,
            "multi_tier_labelled": 0,
            "command_counts": {},
        }

    tests = doc.get("tests", [])
    label_counts: collections.Counter[str] = collections.Counter()
    tier_counts: collections.Counter[str] = collections.Counter()
    command_counts: collections.Counter[str] = collections.Counter()
    unlabelled = 0
    multi_tier = 0

    for test in tests:
        labels = labels_for(test)
        label_counts.update(labels)
        tiers = sorted(set(labels) & TIER_LABELS)
        if not labels:
            unlabelled += 1
        if len(tiers) > 1:
            multi_tier += 1
        tier_counts.update(tiers)
        command_counts[command_name(test)] += 1

    return {
        "available": True,
        "registered": len(tests),
        "label_counts": dict(label_counts.most_common()),
        "tier_label_counts": dict(tier_counts.most_common()),
        "unlabelled": unlabelled,
        "multi_tier_labelled": multi_tier,
        "command_counts": dict(command_counts.most_common(20)),
    }


def _append_regex_fragments(value: str, fragments: list[dict[str, str]], platform: str) -> None:
    for part in value.split("|"):
        part = part.strip()
        if not part:
            continue
        fragments.append({"platform": platform, "pattern": part})


def extract_build_workflow_excludes(workflow: Path) -> list[dict[str, str]]:
    text = workflow.read_text(encoding="utf-8")
    fragments: list[dict[str, str]] = []
    platform = "all"
    in_test_step = False

    for raw_line in text.splitlines():
        line = raw_line.rstrip()
        if "- name: Test (non-Windows)" in line:
            in_test_step = True
            platform = "all"
            continue
        if in_test_step and line.startswith("      - name: ") and "Test (non-Windows)" not in line:
            in_test_step = False
            platform = "all"

        if in_test_step:
            if 'RUNNER_OS}" = "macOS"' in line:
                platform = "macos"
            elif 'RUNNER_OS}" = "Linux"' in line:
                platform = "linux"
            elif line.strip() == "fi":
                platform = "all"

            initial = re.search(r"exclude='([^']*)'", line)
            if initial:
                _append_regex_fragments(initial.group(1), fragments, platform)
                continue

            appended = re.search(r'exclude="\$\{exclude\}\|(.+)"', line)
            if appended:
                _append_regex_fragments(appended.group(1), fragments, platform)
                continue

        windows = re.search(r'--exclude-regex\s+"([^"]*)"', line)
        if windows and "$" not in windows.group(1):
            _append_regex_fragments(windows.group(1), fragments, "windows")

    return fragments


def iter_test_like_files(repo_root: Path) -> Iterable[Path]:
    ignored_dirs = {
        ".git",
        "__pycache__",
        "node_modules",
        "build",
        "build-coverage",
        ".pytest_cache",
    }
    suffixes = {".cpp", ".cc", ".cxx", ".mm", ".m", ".h", ".hpp", ".py", ".ts", ".tsx", ".js", ".mjs", ".swift", ".sh", ".cmake"}

    for root_name in TEST_ROOTS:
        root = repo_root / root_name
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if any(part in ignored_dirs for part in path.parts):
                continue
            if not path.is_file() or path.suffix not in suffixes:
                continue
            if TEST_NAME_RE.search(path.name):
                yield path


def classify_hit(kind: str, text: str, path: Path) -> str:
    lowered = text.lower()
    if "fixture" in path.parts or "fixtures" in path.parts:
        return "fixture"
    if kind == "succeed" and ("skipped" in lowered or "skipping" in lowered):
        return "skip-as-pass"
    if kind == "succeed" and ("placeholder" in lowered or "scaffold" in lowered):
        return "placeholder"
    if kind == "require_true":
        return "unconditional-true"
    return "needs-review"


def scan_weak_patterns(repo_root: Path) -> dict[str, Any]:
    counts: collections.Counter[str] = collections.Counter()
    classifications: collections.Counter[str] = collections.Counter()
    hits: list[PatternHit] = []

    for path in iter_test_like_files(repo_root):
        rel = path.relative_to(repo_root).as_posix()
        try:
            lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
        except OSError:
            continue
        for lineno, line in enumerate(lines, start=1):
            for kind, pattern in WEAK_PATTERNS.items():
                if not pattern.search(line):
                    continue
                classification = classify_hit(kind, line, path)
                counts[kind] += 1
                classifications[classification] += 1
                hits.append(
                    PatternHit(
                        kind=kind,
                        path=rel,
                        line=lineno,
                        text=line.strip(),
                        classification=classification,
                    )
                )

    return {
        "counts": dict(counts),
        "classifications": dict(classifications),
        "hits": [
            {
                "kind": hit.kind,
                "path": hit.path,
                "line": hit.line,
                "text": hit.text,
                "classification": hit.classification,
            }
            for hit in hits
        ],
    }


def _counter_delta(current: dict[str, int], baseline: dict[str, int]) -> dict[str, int]:
    keys = set(current) | set(baseline)
    return {
        key: int(current.get(key, 0)) - int(baseline.get(key, 0))
        for key in sorted(keys)
        if int(current.get(key, 0)) != int(baseline.get(key, 0))
    }


def summarize_weak_pattern_baseline(
    repo_root: Path,
    weak_patterns: dict[str, Any],
) -> dict[str, Any]:
    path = repo_root / WEAK_PATTERN_BASELINE_PATH
    if not path.exists():
        return {
            "available": False,
            "path": WEAK_PATTERN_BASELINE_PATH.as_posix(),
            "count_deltas": {},
            "classification_deltas": {},
        }

    doc = json.loads(path.read_text(encoding="utf-8"))
    baseline_counts = {
        str(key): int(value)
        for key, value in doc.get("counts", {}).items()
    }
    baseline_classifications = {
        str(key): int(value)
        for key, value in doc.get("classifications", {}).items()
    }
    current_counts = {
        str(key): int(value)
        for key, value in weak_patterns.get("counts", {}).items()
    }
    current_classifications = {
        str(key): int(value)
        for key, value in weak_patterns.get("classifications", {}).items()
    }
    return {
        "available": True,
        "path": WEAK_PATTERN_BASELINE_PATH.as_posix(),
        "count_deltas": _counter_delta(current_counts, baseline_counts),
        "classification_deltas": _counter_delta(
            current_classifications,
            baseline_classifications,
        ),
    }


def load_weak_pattern_exemptions(repo_root: Path) -> dict[str, Any]:
    path = repo_root / WEAK_PATTERN_EXEMPTIONS_PATH
    if not path.exists():
        return {
            "available": False,
            "path": WEAK_PATTERN_EXEMPTIONS_PATH.as_posix(),
            "entries": [],
            "invalid_entries": [],
        }

    doc = json.loads(path.read_text(encoding="utf-8"))
    entries = list(doc.get("entries", []))
    invalid: list[dict[str, Any]] = []
    for index, entry in enumerate(entries):
        missing = sorted(WEAK_PATTERN_EXEMPTION_REQUIRED_FIELDS - set(entry))
        if missing:
            invalid.append({"index": index, "missing": missing, "entry": entry})
            continue
        try:
            max_count = int(entry["max_count_impact"])
        except (TypeError, ValueError):
            invalid.append({"index": index, "missing": ["integer max_count_impact"], "entry": entry})
            continue
        if max_count < 0:
            invalid.append({"index": index, "missing": ["non-negative max_count_impact"], "entry": entry})
            continue
        try:
            re.compile(str(entry["text_regex"]))
        except re.error:
            invalid.append({"index": index, "missing": ["valid text_regex"], "entry": entry})

    return {
        "available": True,
        "path": WEAK_PATTERN_EXEMPTIONS_PATH.as_posix(),
        "entries": entries,
        "invalid_entries": invalid,
    }


def weak_hit_matches_exemption(hit: dict[str, Any], entry: dict[str, Any]) -> bool:
    if not fnmatch.fnmatch(str(hit.get("path", "")), str(entry.get("path", ""))):
        return False
    if str(hit.get("kind", "")) != str(entry.get("kind", "")):
        return False
    if str(hit.get("classification", "")) != str(entry.get("classification", "")):
        return False
    text_regex = entry.get("text_regex")
    if text_regex:
        try:
            if not re.search(str(text_regex), str(hit.get("text", ""))):
                return False
        except re.error:
            return False
    return True


def summarize_weak_pattern_exemptions(
    repo_root: Path,
    weak_patterns: dict[str, Any],
) -> dict[str, Any]:
    manifest = load_weak_pattern_exemptions(repo_root)
    entries = manifest["entries"]
    invalid_indexes = {
        int(item["index"])
        for item in manifest["invalid_entries"]
        if "index" in item
    }
    hits = list(weak_patterns.get("hits", []))

    exempted_indexes: set[int] = set()
    entry_matches: dict[int, list[int]] = collections.defaultdict(list)
    for entry_index, entry in enumerate(entries):
        if entry_index in invalid_indexes:
            continue
        for hit_index, hit in enumerate(hits):
            if weak_hit_matches_exemption(hit, entry):
                entry_matches[entry_index].append(hit_index)

    for entry_index, matched_indexes in entry_matches.items():
        entry = entries[entry_index]
        max_count = int(entry.get("max_count_impact", 0))
        sorted_indexes = sorted(
            matched_indexes,
            key=lambda index: (
                str(hits[index].get("path", "")),
                int(hits[index].get("line", 0)),
                str(hits[index].get("text", "")),
            ),
        )
        exempted_indexes.update(sorted_indexes[:max_count])

    exempted_hits = [hits[index] for index in sorted(exempted_indexes)]
    unexempted_hits = [
        hit for index, hit in enumerate(hits)
        if index not in exempted_indexes
    ]
    exempted_classifications = collections.Counter(
        str(hit.get("classification", "<missing>"))
        for hit in exempted_hits
    )
    unexempted_classifications = collections.Counter(
        str(hit.get("classification", "<missing>"))
        for hit in unexempted_hits
    )
    exempted_counts = collections.Counter(
        str(hit.get("kind", "<missing>"))
        for hit in exempted_hits
    )
    unexempted_counts = collections.Counter(
        str(hit.get("kind", "<missing>"))
        for hit in unexempted_hits
    )

    unmatched_entries: list[dict[str, Any]] = []
    over_budget_entries: list[dict[str, Any]] = []
    for index, entry in enumerate(entries):
        if index in invalid_indexes:
            continue
        matches = entry_matches.get(index, [])
        if not matches:
            unmatched_entries.append({"index": index, "entry": entry})
            continue
        max_count = int(entry.get("max_count_impact", 0))
        if len(matches) > max_count:
            over_budget_entries.append({
                "index": index,
                "matched": len(matches),
                "max_count_impact": max_count,
                "entry": entry,
            })

    return {
        "available": manifest["available"],
        "path": manifest["path"],
        "entries": len(entries),
        "invalid_entries": manifest["invalid_entries"],
        "unmatched_entries": unmatched_entries,
        "over_budget_entries": over_budget_entries,
        "exempted_hits": len(exempted_hits),
        "unexempted_hits": len(unexempted_hits),
        "exempted_counts": dict(sorted(exempted_counts.items())),
        "unexempted_counts": dict(sorted(unexempted_counts.items())),
        "exempted_classifications": dict(sorted(exempted_classifications.items())),
        "unexempted_classifications": dict(sorted(unexempted_classifications.items())),
    }


def load_quarantine_manifest(repo_root: Path) -> dict[str, Any]:
    path = repo_root / QUARANTINE_PATH
    if not path.exists():
        return {
            "available": False,
            "path": QUARANTINE_PATH.as_posix(),
            "entries": [],
            "invalid_entries": [],
            "expired_entries": [],
        }

    doc = json.loads(path.read_text(encoding="utf-8"))
    today = _dt.date.today()
    entries = doc.get("entries", [])
    invalid: list[dict[str, Any]] = []
    expired: list[dict[str, Any]] = []

    for index, entry in enumerate(entries):
        missing = sorted(QUARANTINE_REQUIRED_FIELDS - set(entry))
        if missing:
            invalid.append({"index": index, "missing": missing, "entry": entry})
            continue
        try:
            expiry = _dt.date.fromisoformat(str(entry["expires"]))
        except ValueError:
            invalid.append({"index": index, "missing": ["valid expires date"], "entry": entry})
            continue
        if expiry < today:
            expired.append(entry)

    return {
        "available": True,
        "path": QUARANTINE_PATH.as_posix(),
        "entries": entries,
        "invalid_entries": invalid,
        "expired_entries": expired,
    }


def summarize_quarantine(
    workflow_excludes: list[dict[str, str]],
    manifest: dict[str, Any],
) -> dict[str, Any]:
    manifest_keys = {
        (str(entry.get("platform")), str(entry.get("test_pattern")))
        for entry in manifest.get("entries", [])
    }
    missing = [
        item for item in workflow_excludes
        if (item["platform"], item["pattern"]) not in manifest_keys
    ]
    return {
        "available": manifest["available"],
        "path": manifest["path"],
        "entries": len(manifest.get("entries", [])),
        "invalid_entries": manifest.get("invalid_entries", []),
        "expired_entries": manifest.get("expired_entries", []),
        "workflow_excludes_without_manifest": missing,
    }


def load_ci_tiers(repo_root: Path) -> dict[str, Any]:
    path = repo_root / CI_TIERS_PATH
    if not path.exists():
        return {
            "available": False,
            "path": CI_TIERS_PATH.as_posix(),
            "required_status_contexts": [],
            "workflows": [],
            "missing_workflow_files": [],
            "untracked_workflow_files": [],
            "total_workflow_files": 0,
        }

    doc = json.loads(path.read_text(encoding="utf-8"))
    workflows = list(doc.get("workflows", []))
    tracked_paths = {str(item.get("path", "")) for item in workflows}
    all_workflows = sorted(
        path.relative_to(repo_root).as_posix()
        for path in (repo_root / ".github" / "workflows").glob("*.yml")
    )
    missing = [
        str(item.get("path", ""))
        for item in workflows
        if item.get("path") and not (repo_root / str(item["path"])).exists()
    ]
    return {
        "available": True,
        "path": CI_TIERS_PATH.as_posix(),
        "required_status_contexts": list(doc.get("required_status_contexts", [])),
        "workflows": workflows,
        "missing_workflow_files": missing,
        "untracked_workflow_files": [
            workflow for workflow in all_workflows
            if workflow not in tracked_paths
        ],
        "total_workflow_files": len(all_workflows),
    }


def summarize_ci_tiers(repo_root: Path) -> dict[str, Any]:
    manifest = load_ci_tiers(repo_root)
    workflows = manifest["workflows"]
    tier_counts = collections.Counter(str(item.get("tier", "<missing>")) for item in workflows)
    cadence_counts = collections.Counter(str(item.get("cadence", "<missing>")) for item in workflows)
    platform_counts: collections.Counter[str] = collections.Counter()
    for workflow in workflows:
        for platform in workflow.get("platforms", []):
            platform_counts[str(platform)] += 1

    return {
        "available": manifest["available"],
        "path": manifest["path"],
        "required_status_contexts": manifest["required_status_contexts"],
        "workflows": workflows,
        "workflow_count": len(workflows),
        "total_workflow_files": manifest["total_workflow_files"],
        "missing_workflow_files": manifest["missing_workflow_files"],
        "untracked_workflow_files": manifest["untracked_workflow_files"],
        "tier_counts": dict(sorted(tier_counts.items())),
        "cadence_counts": dict(sorted(cadence_counts.items())),
        "platform_counts": dict(sorted(platform_counts.items())),
    }


def load_module_coverage_manifest(repo_root: Path) -> dict[str, Any]:
    path = repo_root / MODULE_COVERAGE_PATH
    if not path.exists():
        return {
            "available": False,
            "path": MODULE_COVERAGE_PATH.as_posix(),
            "modules": [],
            "invalid_modules": [],
        }

    doc = json.loads(path.read_text(encoding="utf-8"))
    modules = list(doc.get("modules", []))
    required_fields = {
        "id",
        "owner",
        "title",
        "guarantees",
        "representative_tests",
        "missing_or_weak",
        "command_patterns",
    }
    invalid: list[dict[str, Any]] = []
    for index, module in enumerate(modules):
        missing = sorted(required_fields - set(module))
        if missing:
            invalid.append({"index": index, "missing": missing, "module": module})

    return {
        "available": True,
        "path": MODULE_COVERAGE_PATH.as_posix(),
        "modules": modules,
        "invalid_modules": invalid,
    }


CompiledModulePatterns = list[
    tuple[dict[str, Any], list[re.Pattern[str]], list[re.Pattern[str]]]
]


def compile_module_patterns(modules: list[dict[str, Any]]) -> CompiledModulePatterns:
    compiled: CompiledModulePatterns = []
    for module in modules:
        command_patterns = [
            re.compile(str(pattern), re.IGNORECASE)
            for pattern in module.get("command_patterns", [])
        ]
        test_name_patterns = [
            re.compile(str(pattern), re.IGNORECASE)
            for pattern in module.get("test_name_patterns", [])
        ]
        compiled.append((module, command_patterns, test_name_patterns))
    return compiled


def module_pattern_matches(
    test: dict[str, Any],
    compiled: CompiledModulePatterns,
) -> list[str]:
    command = command_name(test)
    name = str(test.get("name", ""))
    matches: list[str] = []
    for module, command_patterns, test_name_patterns in compiled:
        if any(pattern.search(command) for pattern in command_patterns) or any(
            pattern.search(name) for pattern in test_name_patterns
        ):
            matches.append(str(module.get("id", "<missing>")))
    return matches


def module_unmapped_command_counts(
    ctest_doc: dict[str, Any] | None,
    modules: list[dict[str, Any]],
) -> collections.Counter[str]:
    counts: collections.Counter[str] = collections.Counter()
    if ctest_doc is None:
        return counts

    compiled = compile_module_patterns(modules)
    for test in ctest_doc.get("tests", []):
        if not module_pattern_matches(test, compiled):
            counts[command_name(test)] += 1
    return counts


def load_module_unmapped_triage_manifest(repo_root: Path) -> dict[str, Any]:
    path = repo_root / MODULE_COVERAGE_UNMAPPED_TRIAGE_PATH
    if not path.exists():
        return {
            "available": False,
            "path": MODULE_COVERAGE_UNMAPPED_TRIAGE_PATH.as_posix(),
            "entries": [],
            "invalid_entries": [],
        }

    doc = json.loads(path.read_text(encoding="utf-8"))
    entries = list(doc.get("entries", []))
    invalid: list[dict[str, Any]] = []
    seen_commands: set[str] = set()
    for index, entry in enumerate(entries):
        missing = sorted(MODULE_COVERAGE_UNMAPPED_TRIAGE_REQUIRED_FIELDS - set(entry))
        expected_count = entry.get("expected_count")
        if (
            not isinstance(expected_count, int)
            or isinstance(expected_count, bool)
            or expected_count < 0
        ):
            missing.append("expected_count:int>=0")
        command = entry.get("command")
        if isinstance(command, str):
            if command in seen_commands:
                missing.append("duplicate_command")
            seen_commands.add(command)
        if missing:
            invalid.append({"index": index, "missing": sorted(set(missing)), "entry": entry})

    return {
        "available": True,
        "path": MODULE_COVERAGE_UNMAPPED_TRIAGE_PATH.as_posix(),
        "entries": entries,
        "invalid_entries": invalid,
    }


def summarize_module_unmapped_triage(
    repo_root: Path,
    ctest_doc: dict[str, Any] | None,
    modules: list[dict[str, Any]],
) -> dict[str, Any]:
    manifest = load_module_unmapped_triage_manifest(repo_root)
    counts = module_unmapped_command_counts(ctest_doc, modules)
    valid_entries = [
        entry
        for index, entry in enumerate(manifest["entries"])
        if all(invalid["index"] != index for invalid in manifest["invalid_entries"])
    ]

    triaged = 0
    count_mismatches: list[dict[str, Any]] = []
    stale_entries: list[dict[str, Any]] = []
    covered_commands: set[str] = set()

    for entry in valid_entries:
        command = str(entry["command"])
        expected = int(entry["expected_count"])
        actual = counts.get(command, 0)
        covered_commands.add(command)
        triaged += min(actual, expected)
        if actual == 0:
            stale_entries.append({
                "command": command,
                "expected_count": expected,
                "owner": entry.get("owner", "<missing>"),
            })
        if actual != expected:
            count_mismatches.append({
                "command": command,
                "expected_count": expected,
                "actual_count": actual,
                "owner": entry.get("owner", "<missing>"),
            })

    untriaged_commands = {
        command: count
        for command, count in sorted(counts.items())
        if command not in covered_commands
    }
    over_expected = {
        command: count - int(entry["expected_count"])
        for entry in valid_entries
        for command, count in [(str(entry["command"]), counts.get(str(entry["command"]), 0))]
        if count > int(entry["expected_count"])
    }
    untriaged_total = sum(untriaged_commands.values()) + sum(over_expected.values())

    return {
        "available": manifest["available"],
        "path": manifest["path"],
        "entries": len(manifest["entries"]),
        "invalid_entries": manifest["invalid_entries"],
        "unmapped_registered_tests": sum(counts.values()),
        "triaged_unmapped_tests": triaged,
        "untriaged_unmapped_tests": untriaged_total,
        "untriaged_commands": untriaged_commands,
        "over_expected_commands": over_expected,
        "count_mismatches": count_mismatches,
        "stale_entries": stale_entries,
        "command_counts": dict(sorted(counts.items())),
    }


def summarize_module_coverage(
    repo_root: Path,
    ctest_doc: dict[str, Any] | None,
) -> dict[str, Any]:
    manifest = load_module_coverage_manifest(repo_root)
    modules = manifest["modules"]
    if ctest_doc is None:
        return {
            "available": manifest["available"],
            "path": manifest["path"],
            "module_count": len(modules),
            "invalid_modules": manifest["invalid_modules"],
            "modules": [],
            "registered_tests": 0,
            "mapped_registered_tests": 0,
            "unmapped_registered_tests": 0,
            "unmapped_triage": summarize_module_unmapped_triage(
                repo_root,
                None,
                modules,
            ),
        }

    tests = list(ctest_doc.get("tests", []))
    compiled = compile_module_patterns(modules)

    module_matches: dict[str, int] = {str(module.get("id", "<missing>")): 0 for module in modules}
    unmapped = 0

    for test in tests:
        matches = module_pattern_matches(test, compiled)
        for module_id in matches:
            module_matches[module_id] += 1
        if not matches:
            unmapped += 1

    module_summaries: list[dict[str, Any]] = []
    for module in modules:
        module_id = str(module.get("id", "<missing>"))
        module_summaries.append({
            "id": module_id,
            "owner": module.get("owner", "<missing>"),
            "title": module.get("title", module_id),
            "registered_tests": module_matches.get(module_id, 0),
            "guarantee_count": len(module.get("guarantees", [])),
            "representative_tests": module.get("representative_tests", []),
            "missing_or_weak": module.get("missing_or_weak", []),
        })

    return {
        "available": manifest["available"],
        "path": manifest["path"],
        "module_count": len(modules),
        "invalid_modules": manifest["invalid_modules"],
        "modules": module_summaries,
        "registered_tests": len(tests),
        "mapped_registered_tests": len(tests) - unmapped,
        "unmapped_registered_tests": unmapped,
        "unmapped_triage": summarize_module_unmapped_triage(
            repo_root,
            ctest_doc,
            modules,
        ),
    }


def load_capability_manifest(repo_root: Path) -> dict[str, Any]:
    path = repo_root / CAPABILITY_LABELS_PATH
    if not path.exists():
        return {
            "available": False,
            "path": CAPABILITY_LABELS_PATH.as_posix(),
            "capabilities": [],
            "invalid_capabilities": [],
        }

    doc = json.loads(path.read_text(encoding="utf-8"))
    capabilities = list(doc.get("capabilities", []))
    invalid: list[dict[str, Any]] = []
    for index, capability in enumerate(capabilities):
        missing = sorted(CAPABILITY_REQUIRED_FIELDS - set(capability))
        if missing:
            invalid.append({
                "index": index,
                "missing": missing,
                "capability": capability,
            })

    return {
        "available": True,
        "path": CAPABILITY_LABELS_PATH.as_posix(),
        "capabilities": capabilities,
        "invalid_capabilities": invalid,
    }


def summarize_capabilities(
    repo_root: Path,
    ctest_doc: dict[str, Any] | None,
) -> dict[str, Any]:
    manifest = load_capability_manifest(repo_root)
    capabilities = manifest["capabilities"]
    if ctest_doc is None:
        return {
            "available": manifest["available"],
            "path": manifest["path"],
            "capability_count": len(capabilities),
            "invalid_capabilities": manifest["invalid_capabilities"],
            "registered_tests": 0,
            "tests_with_capability": 0,
            "tests_without_capability": 0,
            "capabilities": [],
        }

    tests = list(ctest_doc.get("tests", []))
    compiled: list[tuple[dict[str, Any], list[re.Pattern[str]], list[re.Pattern[str]], set[str]]] = []
    for capability in capabilities:
        command_patterns = [
            re.compile(str(pattern), re.IGNORECASE)
            for pattern in capability.get("command_patterns", [])
        ]
        test_name_patterns = [
            re.compile(str(pattern), re.IGNORECASE)
            for pattern in capability.get("test_name_patterns", [])
        ]
        label_patterns = {
            str(label).lower()
            for label in capability.get("label_patterns", [])
        }
        compiled.append((capability, command_patterns, test_name_patterns, label_patterns))

    matches: dict[str, int] = {
        str(capability.get("id", "<missing>")): 0
        for capability in capabilities
    }
    tests_with_capability = 0

    for test in tests:
        command = command_name(test)
        name = str(test.get("name", ""))
        labels = {label.lower() for label in labels_for(test)}
        matched = False
        for capability, command_patterns, test_name_patterns, label_patterns in compiled:
            if (
                any(pattern.search(command) for pattern in command_patterns)
                or any(pattern.search(name) for pattern in test_name_patterns)
                or bool(labels & label_patterns)
            ):
                matches[str(capability.get("id", "<missing>"))] += 1
                matched = True
        if matched:
            tests_with_capability += 1

    capability_summaries: list[dict[str, Any]] = []
    for capability in capabilities:
        capability_id = str(capability.get("id", "<missing>"))
        capability_summaries.append({
            "id": capability_id,
            "title": capability.get("title", capability_id),
            "owner": capability.get("owner", "<missing>"),
            "registered_tests": matches.get(capability_id, 0),
            "expected_behavior": capability.get("expected_behavior", "<missing>"),
            "missing_or_weak": capability.get("missing_or_weak", []),
        })

    return {
        "available": manifest["available"],
        "path": manifest["path"],
        "capability_count": len(capabilities),
        "invalid_capabilities": manifest["invalid_capabilities"],
        "registered_tests": len(tests),
        "tests_with_capability": tests_with_capability,
        "tests_without_capability": len(tests) - tests_with_capability,
        "capabilities": capability_summaries,
    }


def build_report(
    repo_root: Path,
    ctest_doc: dict[str, Any] | None,
    build_dir: Path | None = None,
) -> dict[str, Any]:
    workflow_excludes = extract_build_workflow_excludes(
        repo_root / ".github" / "workflows" / "build.yml"
    )
    exclude_counts = collections.Counter(item["platform"] for item in workflow_excludes)
    quarantine = load_quarantine_manifest(repo_root)
    weak_patterns = scan_weak_patterns(repo_root)
    weak_pattern_exemptions = summarize_weak_pattern_exemptions(
        repo_root,
        weak_patterns,
    )
    weak_patterns_for_baseline = {
        "counts": weak_pattern_exemptions["unexempted_counts"],
        "classifications": weak_pattern_exemptions["unexempted_classifications"],
    }
    return {
        "ctest": summarize_ctest(ctest_doc),
        "ctest_execution": summarize_ctest_execution(build_dir),
        "workflow_excludes": {
            "count": len(workflow_excludes),
            "by_platform": dict(sorted(exclude_counts.items())),
            "patterns": workflow_excludes,
        },
        "quarantine": summarize_quarantine(workflow_excludes, quarantine),
        "weak_patterns": weak_patterns,
        "weak_pattern_baseline": summarize_weak_pattern_baseline(
            repo_root,
            weak_patterns_for_baseline,
        ),
        "weak_pattern_exemptions": weak_pattern_exemptions,
        "ci_tiers": summarize_ci_tiers(repo_root),
        "module_coverage": summarize_module_coverage(repo_root, ctest_doc),
        "capabilities": summarize_capabilities(repo_root, ctest_doc),
    }


def render_markdown(report: dict[str, Any]) -> str:
    ctest = report["ctest"]
    excludes = report["workflow_excludes"]
    weak = report["weak_patterns"]
    lines: list[str] = [
        "# Test Suite Quality Baseline",
        "",
        "## CTest Registration",
        "",
    ]
    if ctest["available"]:
        lines.extend([
            f"- Registered tests: {ctest['registered']}",
            f"- Unlabelled tests: {ctest['unlabelled']}",
            f"- Tests with multiple tier labels: {ctest['multi_tier_labelled']}",
            "",
            "Top commands:",
            "",
        ])
        for name, count in list(ctest["command_counts"].items())[:10]:
            lines.append(f"- `{name}`: {count}")
        lines.extend(["", "Tier labels:", ""])
        if ctest["tier_label_counts"]:
            for name, count in ctest["tier_label_counts"].items():
                lines.append(f"- `{name}`: {count}")
        else:
            lines.append("- No tier labels found")
    else:
        lines.append("- CTest JSON unavailable; pass `--ctest-json` or `--build-dir`.")

    execution = report["ctest_execution"]
    lines.extend([
        "",
        "## CTest Execution Log",
        "",
        f"- Available: {str(execution['available']).lower()}",
        f"- Selected tests in last log: {execution['selected']}",
        f"- Executed tests in last log: {execution['executed']}",
        f"- Passed: {execution['passed']}",
        f"- Failed: {execution['failed']}",
        f"- Skipped/not-run: {execution['skipped']}",
        f"- Disabled entries: {execution['disabled']}",
        f"- LastTestsFailed entries: {execution['failed_log_entries']}",
    ])

    lines.extend([
        "",
        "## Workflow Exclusions",
        "",
        f"- Inline exclude fragments: {excludes['count']}",
    ])
    for platform, count in excludes["by_platform"].items():
        lines.append(f"- `{platform}`: {count}")

    quarantine = report["quarantine"]
    lines.extend([
        "",
        "## Quarantine Manifest",
        "",
        f"- Manifest: `{quarantine['path']}`",
        f"- Available: {str(quarantine['available']).lower()}",
        f"- Entries: {quarantine['entries']}",
        f"- Invalid entries: {len(quarantine['invalid_entries'])}",
        f"- Expired entries: {len(quarantine['expired_entries'])}",
        "- Workflow excludes without manifest entry: "
        f"{len(quarantine['workflow_excludes_without_manifest'])}",
    ])

    lines.extend([
        "",
        "## Weak-Test Pattern Scan",
        "",
    ])
    for key in ("succeed", "require_true", "skip"):
        lines.append(f"- `{key}`: {weak['counts'].get(key, 0)}")
    lines.append("")
    lines.append("Classifications:")
    lines.append("")
    if weak["classifications"]:
        for name, count in sorted(weak["classifications"].items()):
            lines.append(f"- `{name}`: {count}")
    else:
        lines.append("- No weak-test patterns found")

    weak_baseline = report["weak_pattern_baseline"]
    lines.extend([
        "",
        "## Weak-Test Baseline",
        "",
        f"- Baseline: `{weak_baseline['path']}`",
        f"- Available: {str(weak_baseline['available']).lower()}",
        "- Comparison scope: unexempted weak-pattern hits",
        f"- Count deltas: {len(weak_baseline['count_deltas'])}",
        f"- Classification deltas: {len(weak_baseline['classification_deltas'])}",
    ])
    if weak_baseline["count_deltas"]:
        lines.append("")
        lines.append("Count deltas:")
        lines.append("")
        for name, delta in weak_baseline["count_deltas"].items():
            lines.append(f"- `{name}`: {delta:+d}")
    if weak_baseline["classification_deltas"]:
        lines.append("")
        lines.append("Classification deltas:")
        lines.append("")
        for name, delta in weak_baseline["classification_deltas"].items():
            lines.append(f"- `{name}`: {delta:+d}")

    weak_exemptions = report["weak_pattern_exemptions"]
    lines.extend([
        "",
        "## Weak-Test Exemptions",
        "",
        f"- Manifest: `{weak_exemptions['path']}`",
        f"- Available: {str(weak_exemptions['available']).lower()}",
        f"- Entries: {weak_exemptions['entries']}",
        f"- Invalid entries: {len(weak_exemptions['invalid_entries'])}",
        f"- Unmatched entries: {len(weak_exemptions['unmatched_entries'])}",
        f"- Over-budget entries: {len(weak_exemptions['over_budget_entries'])}",
        f"- Exempted weak-pattern hits: {weak_exemptions['exempted_hits']}",
        f"- Unexempted weak-pattern hits: {weak_exemptions['unexempted_hits']}",
    ])
    if weak_exemptions["exempted_classifications"]:
        lines.append("")
        lines.append("Exempted classifications:")
        lines.append("")
        for name, count in weak_exemptions["exempted_classifications"].items():
            lines.append(f"- `{name}`: {count}")
    if weak_exemptions["unexempted_classifications"]:
        lines.append("")
        lines.append("Unexempted classifications:")
        lines.append("")
        for name, count in weak_exemptions["unexempted_classifications"].items():
            lines.append(f"- `{name}`: {count}")

    ci_tiers = report["ci_tiers"]
    lines.extend([
        "",
        "## CI Test Signal Inventory",
        "",
        f"- Manifest: `{ci_tiers['path']}`",
        f"- Available: {str(ci_tiers['available']).lower()}",
        f"- Curated workflows tracked: {ci_tiers['workflow_count']}",
        f"- Total workflow files: {ci_tiers['total_workflow_files']}",
        f"- Missing workflow files: {len(ci_tiers['missing_workflow_files'])}",
        "- Workflow files outside curated inventory: "
        f"{len(ci_tiers['untracked_workflow_files'])}",
    ])
    if ci_tiers["required_status_contexts"]:
        lines.append("- Required status contexts: " + ", ".join(
            f"`{item.get('name', '<missing>')}`"
            for item in ci_tiers["required_status_contexts"]
        ))
    if ci_tiers["tier_counts"]:
        lines.append("")
        lines.append("Workflow tiers:")
        lines.append("")
        for name, count in ci_tiers["tier_counts"].items():
            lines.append(f"- `{name}`: {count}")
    if ci_tiers["platform_counts"]:
        lines.append("")
        lines.append("Workflow platform coverage:")
        lines.append("")
        for name, count in ci_tiers["platform_counts"].items():
            lines.append(f"- `{name}`: {count}")

    module_coverage = report["module_coverage"]
    lines.extend([
        "",
        "## Module Coverage Map",
        "",
        f"- Manifest: `{module_coverage['path']}`",
        f"- Available: {str(module_coverage['available']).lower()}",
        f"- Modules tracked: {module_coverage['module_count']}",
        f"- Invalid module entries: {len(module_coverage['invalid_modules'])}",
        f"- Registered tests considered: {module_coverage['registered_tests']}",
        f"- Registered tests matched by at least one module: {module_coverage['mapped_registered_tests']}",
        "- Registered tests outside current module map: "
        f"{module_coverage['unmapped_registered_tests']}",
        "- Per-module counts can overlap when a test covers multiple product areas.",
    ])
    if module_coverage["modules"]:
        lines.append("")
        lines.append("Modules:")
        lines.append("")
        for module in module_coverage["modules"]:
            lines.append(
                f"- `{module['id']}` ({module['owner']}): "
                f"{module['registered_tests']} matched tests, "
                f"{module['guarantee_count']} guarantees, "
                f"{len(module['missing_or_weak'])} known gaps"
            )

    triage = module_coverage["unmapped_triage"]
    lines.extend([
        "",
        "## Module Coverage Unmapped Triage",
        "",
        f"- Manifest: `{triage['path']}`",
        f"- Available: {str(triage['available']).lower()}",
        f"- Entries: {triage['entries']}",
        f"- Invalid entries: {len(triage['invalid_entries'])}",
        f"- Unmapped registered tests: {triage['unmapped_registered_tests']}",
        f"- Triaged unmapped tests: {triage['triaged_unmapped_tests']}",
        f"- Untriaged unmapped tests: {triage['untriaged_unmapped_tests']}",
        f"- Count mismatches: {len(triage['count_mismatches'])}",
        f"- Stale entries: {len(triage['stale_entries'])}",
    ])
    if triage["untriaged_commands"]:
        lines.append("")
        lines.append("Untriaged commands:")
        lines.append("")
        for command, count in triage["untriaged_commands"].items():
            lines.append(f"- `{command or '<empty>'}`: {count}")
    if triage["over_expected_commands"]:
        lines.append("")
        lines.append("Over-expected triaged commands:")
        lines.append("")
        for command, count in triage["over_expected_commands"].items():
            lines.append(f"- `{command or '<empty>'}`: {count}")
    if triage["count_mismatches"]:
        lines.append("")
        lines.append("Count mismatches:")
        lines.append("")
        for item in triage["count_mismatches"]:
            lines.append(
                f"- `{item['command'] or '<empty>'}` "
                f"({item['owner']}): expected {item['expected_count']}, "
                f"actual {item['actual_count']}"
            )
    if triage["stale_entries"]:
        lines.append("")
        lines.append("Stale entries:")
        lines.append("")
        for item in triage["stale_entries"]:
            lines.append(
                f"- `{item['command'] or '<empty>'}` "
                f"({item['owner']}): expected {item['expected_count']}, actual 0"
            )

    capabilities = report["capabilities"]
    lines.extend([
        "",
        "## Capability Map",
        "",
        f"- Manifest: `{capabilities['path']}`",
        f"- Available: {str(capabilities['available']).lower()}",
        f"- Capabilities tracked: {capabilities['capability_count']}",
        f"- Invalid capability entries: {len(capabilities['invalid_capabilities'])}",
        f"- Registered tests considered: {capabilities['registered_tests']}",
        f"- Registered tests matched by at least one capability: {capabilities['tests_with_capability']}",
        "- Registered tests without inferred capability: "
        f"{capabilities['tests_without_capability']}",
        "- Capability counts can overlap and are report-only until real CTest labels are applied.",
    ])
    if capabilities["capabilities"]:
        lines.append("")
        lines.append("Capabilities:")
        lines.append("")
        for capability in capabilities["capabilities"]:
            lines.append(
                f"- `{capability['id']}` ({capability['owner']}): "
                f"{capability['registered_tests']} matched tests; "
                f"{capability['expected_behavior']}"
            )

    return "\n".join(lines) + "\n"


def enforcement_findings(
    report: dict[str, Any],
    *,
    fail_on_quarantine_drift: bool,
    fail_on_weak_pattern_growth: bool,
    fail_on_expired_quarantine: bool,
    fail_on_unmapped_triage_drift: bool,
) -> list[str]:
    findings: list[str] = []
    quarantine = report["quarantine"]
    weak_baseline = report["weak_pattern_baseline"]
    unmapped_triage = report["module_coverage"]["unmapped_triage"]

    if fail_on_quarantine_drift:
        missing = quarantine["workflow_excludes_without_manifest"]
        invalid = quarantine["invalid_entries"]
        if missing:
            findings.append(
                "workflow exclusions missing quarantine metadata: "
                f"{len(missing)}"
            )
        if invalid:
            findings.append(f"invalid quarantine entries: {len(invalid)}")

    if fail_on_expired_quarantine and quarantine["expired_entries"]:
        findings.append(
            "expired quarantine entries: "
            f"{len(quarantine['expired_entries'])}"
        )

    if fail_on_weak_pattern_growth:
        growth = {
            key: value
            for key, value in weak_baseline["count_deltas"].items()
            if value > 0
        }
        classification_growth = {
            key: value
            for key, value in weak_baseline["classification_deltas"].items()
            if value > 0
        }
        if growth:
            findings.append(f"weak-test count growth: {growth}")
        if classification_growth:
            findings.append(
                f"weak-test classification growth: {classification_growth}"
            )

    if fail_on_unmapped_triage_drift:
        if unmapped_triage["invalid_entries"]:
            findings.append(
                "invalid module-unmapped triage entries: "
                f"{len(unmapped_triage['invalid_entries'])}"
            )
        if unmapped_triage["untriaged_unmapped_tests"]:
            findings.append(
                "untriaged module-unmapped tests: "
                f"{unmapped_triage['untriaged_unmapped_tests']}"
            )
        if unmapped_triage["count_mismatches"]:
            findings.append(
                "module-unmapped triage count mismatches: "
                f"{len(unmapped_triage['count_mismatches'])}"
            )
        if unmapped_triage["stale_entries"]:
            findings.append(
                "stale module-unmapped triage entries: "
                f"{len(unmapped_triage['stale_entries'])}"
            )

    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=None)
    parser.add_argument("--ctest-json", type=Path, default=None)
    parser.add_argument("--build-dir", type=Path, default=None,
                        help="Run ctest --show-only=json-v1 against this build dir.")
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--fail-on-quarantine-drift", action="store_true",
                        help="Exit non-zero when workflow exclusions lack valid quarantine metadata.")
    parser.add_argument("--fail-on-expired-quarantine", action="store_true",
                        help="Exit non-zero when quarantine entries are expired.")
    parser.add_argument("--fail-on-weak-pattern-growth", action="store_true",
                        help="Exit non-zero when weak-test counts grow above the baseline.")
    parser.add_argument("--fail-on-unmapped-triage-drift", action="store_true",
                        help="Exit non-zero when module-unmapped triage has invalid, stale, mismatched, or untriaged entries.")
    args = parser.parse_args(argv)

    repo_root = args.repo_root or repo_root_from(Path.cwd())
    build_dir = args.build_dir
    if build_dir is not None and not build_dir.is_absolute():
        build_dir = repo_root / build_dir

    ctest_doc = load_ctest_json(args.ctest_json, build_dir)
    report = build_report(repo_root, ctest_doc, build_dir)
    if args.format == "json":
        rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    else:
        rendered = render_markdown(report)

    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    else:
        sys.stdout.write(rendered)

    findings = enforcement_findings(
        report,
        fail_on_quarantine_drift=args.fail_on_quarantine_drift,
        fail_on_weak_pattern_growth=args.fail_on_weak_pattern_growth,
        fail_on_expired_quarantine=args.fail_on_expired_quarantine,
        fail_on_unmapped_triage_drift=args.fail_on_unmapped_triage_drift,
    )
    if findings:
        for finding in findings:
            print(f"test_suite_quality_report: {finding}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
