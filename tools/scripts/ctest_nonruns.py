#!/usr/bin/env python3
"""Observe a CTest JUnit artifact; never decide whether a test skip is allowed."""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import os
from pathlib import Path
import stat
import sys
import xml.etree.ElementTree as ET

MAX_BYTES = 64 * 1024 * 1024
MAX_ROWS = 100
STATUSES = ("run", "fail", "notrun", "disabled")


class CTestTree(ET.TreeBuilder):
    """Reject entity declarations before expansion, including UTF-16 input."""

    def doctype(self, name, pubid, system):
        raise ValueError("Document types are not supported")


def bounded_text(value: str, limit: int = 512) -> str:
    value = "".join(c for c in value if c.isprintable() or c == "\n")
    return value[:limit] + ("…" if len(value) > limit else "")


def read_report(path: Path) -> bytes:
    # O_NONBLOCK prevents a regular-file-to-FIFO replacement from blocking open.
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NONBLOCK", 0))
    with os.fdopen(fd, "rb") as stream:
        metadata = os.fstat(stream.fileno())
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError("Input must be a regular file")
        if metadata.st_size > MAX_BYTES:
            raise ValueError("Input exceeds the 64 MiB limit")
        data = stream.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ValueError("Input grew beyond the 64 MiB limit")
    return data


def observe_with_cases(path: Path, registered=None, raw_lines=None) -> tuple[dict, list[dict]]:
    report = {
        "schema": "pulp.ctest-nonruns.v2", "observation": "unavailable",
        "input": str(path), "sha256": None, "bytes": None,
        "registered": registered, "listing_raw_lines": raw_lines,
        "declared_tests": None, "testcases": None, "counts": None,
        "nonruns": [], "omitted_nonruns": 0, "comparison": None, "issues": [],
        "limitations": [
            "Describes only the supplied artifact, not current-head or loaded-binary proof.",
            "Registered count is caller-supplied context, not a selection or provenance check.",
            "Filtered and configure-time absent tests are not identifiable from this report.",
            "Observer exit status is not the CTest exit status or a skip-policy verdict.",
        ],
    }
    identities = []
    try:
        data = read_report(path)
        report.update(sha256=hashlib.sha256(data).hexdigest(), bytes=len(data))
        suite = ET.fromstring(data, parser=ET.XMLParser(target=CTestTree()))
        if suite.tag != "testsuite":
            raise ValueError("Expected a CTest testsuite root (not generic JUnit)")
        cases = suite.findall("testcase")
        if len(list(suite.iter("testcase"))) != len(cases):
            raise ValueError("Nested testcases are not CTest output")
        declared = suite.get("tests", "")
        if not declared.isascii() or not declared.isdecimal() or len(declared) > 9:
            raise ValueError("Missing or invalid tests count")
        report.update(declared_tests=int(declared), testcases=len(cases))
        counts = dict.fromkeys((*STATUSES, "unknown"), 0)
        name_occurrences = {}
        rows = []
        for index, case in enumerate(cases):
            status = case.get("status", "")
            raw_name = case.get("name", "")
            occurrence = name_occurrences.get(raw_name, 0) + 1
            name_occurrences[raw_name] = occurrence
            identities.append({
                "name": bounded_text(raw_name),
                "name_sha256": hashlib.sha256(raw_name.encode("utf-8")).hexdigest(),
                "occurrence": occurrence,
                "status": status,
            })
            counts[status if status in STATUSES else "unknown"] += 1
            if status not in STATUSES or not case.get("name"):
                report["issues"].append(f"Testcase {index + 1} has unknown status or no name")
            failure = case.find("failure") is not None or case.find("error") is not None
            skipped = case.find("skipped")
            if (failure and status != "fail") or (skipped is not None and status not in ("notrun", "disabled")):
                report["issues"].append(f"Testcase {index + 1} has contradictory outcome fields")
            if status not in ("notrun", "disabled"):
                continue
            if len(rows) == MAX_ROWS:
                report["omitted_nonruns"] += 1
                continue
            labels = [p.get("value", "") for p in case.iterfind("properties/property") if p.get("name") == "cmake_labels"]
            output = (case.findtext("system-out") or "").strip().splitlines()
            rows.append({
                "index": index + 1, "name": bounded_text(case.get("name", "")),
                "status": status,
                "reason": bounded_text((skipped.get("message") if skipped is not None else None) or status),
                "labels": bounded_text(";".join(labels)),
                "output": bounded_text(output[-1] if output else "", 160),
            })
        report.update(counts=counts, nonruns=rows)
        for field, status in (("failures", "fail"), ("disabled", "disabled"), ("skipped", "notrun")):
            value = suite.get(field)
            if value is not None and (not value.isascii() or not value.isdecimal() or len(value) > 9 or int(value) != counts[status]):
                report["issues"].append(f"Declared {field} count is invalid or differs from testcase statuses")
        if report["declared_tests"] != len(cases):
            report["issues"].append("Declared tests count differs from testcase population")
        if not cases:
            report["issues"].append("No testcase entries; this artifact provides no evidence that tests ran")
        report["observation"] = "incomplete" if report["issues"] else "observed"
    except FileNotFoundError:
        report["issues"].append("No JUnit report; the job may not have reached CTest or the report path may be wrong")
    except (OSError, ValueError, LookupError, ET.ParseError) as error:
        report["issues"].append(bounded_text(str(error)))
    report["omitted_issues"] = max(0, len(report["issues"]) - MAX_ROWS)
    report["issues"] = report["issues"][:MAX_ROWS]
    return report, identities


def observe(path: Path, registered=None, raw_lines=None) -> dict:
    return observe_with_cases(path, registered, raw_lines)[0]


def compare(report: dict, cases: list[dict], baseline_path: Path) -> None:
    baseline, baseline_cases = observe_with_cases(baseline_path)
    if report["observation"] != "observed" or baseline["observation"] != "observed":
        report["issues"].append(
            "Comparison requires internally consistent current and baseline CTest artifacts"
        )
        report["observation"] = "incomplete"
        report["omitted_issues"] += max(0, len(report["issues"]) - MAX_ROWS)
        report["issues"] = report["issues"][:MAX_ROWS]
        report["comparison"] = {
            "baseline": {
                "input": str(baseline_path), "sha256": baseline["sha256"],
                "observation": baseline["observation"], "issues": baseline["issues"],
                "omitted_issues": baseline["omitted_issues"],
            },
            "status": "unavailable",
        }
        return

    def grouped(rows):
        result = {}
        for row in rows:
            result.setdefault(row["name_sha256"], []).append(row)
        return result

    def status_counts(rows):
        counts = dict.fromkeys((*STATUSES, "unknown"), 0)
        for row in rows:
            status = row["status"] if row["status"] in STATUSES else "unknown"
            counts[status] += 1
        return counts

    current_by_name = grouped(cases)
    baseline_by_name = grouped(baseline_cases)
    shared = current_by_name.keys() & baseline_by_name.keys()
    transitions = []
    ambiguous_duplicate_groups = []
    transition_counts = {
        "newly_failed": 0, "new_nonrun": 0, "changed": 0,
        "failure_cleared": 0, "recovered": 0,
        "ambiguous_duplicate_groups": 0,
    }
    nonrun = {"notrun", "disabled"}
    for key in sorted(shared):
        before_group = baseline_by_name[key]
        after_group = current_by_name[key]
        if len(before_group) != 1 or len(after_group) != 1:
            before_counts = status_counts(before_group)
            after_counts = status_counts(after_group)
            if before_counts == after_counts:
                continue
            transition_counts["ambiguous_duplicate_groups"] += 1
            ambiguous_duplicate_groups.append({
                "name": after_group[0]["name"], "name_sha256": key,
                "baseline_count": len(before_group), "current_count": len(after_group),
                "before_status_counts": before_counts,
                "after_status_counts": after_counts,
                "kind": "ambiguous_duplicate_group",
            })
            continue
        before = before_group[0]
        after = after_group[0]
        if before["status"] == after["status"]:
            continue
        if after["status"] == "fail" and before["status"] != "fail":
            kind = "newly_failed"
        elif after["status"] in nonrun and before["status"] not in nonrun:
            kind = "new_nonrun"
        elif before["status"] == "fail" and after["status"] == "run":
            kind = "failure_cleared"
        elif before["status"] in nonrun and after["status"] == "run":
            kind = "recovered"
        else:
            kind = "changed"
        transition_counts[kind] += 1
        transitions.append({
            "name": after["name"], "name_sha256": after["name_sha256"],
            "before": before["status"], "after": after["status"], "kind": kind,
        })

    def unmatched(keys, groups):
        return [{
            "name": groups[key][0]["name"], "name_sha256": key,
            "count": len(groups[key]), "status_counts": status_counts(groups[key]),
        } for key in sorted(keys)]

    current_only = unmatched(current_by_name.keys() - baseline_by_name.keys(), current_by_name)
    baseline_only = unmatched(baseline_by_name.keys() - current_by_name.keys(), baseline_by_name)
    candidates = []
    transition_priority = {
        "newly_failed": 0, "new_nonrun": 0, "changed": 1,
        "failure_cleared": 4, "recovered": 5,
    }
    candidates.extend((transition_priority[row["kind"]], row["name_sha256"], "transitions", row)
                      for row in transitions)
    candidates.extend((2, row["name_sha256"], "ambiguous", row)
                      for row in ambiguous_duplicate_groups)
    for row in current_only:
        statuses = row["status_counts"]
        priority = 0 if statuses["notrun"] or statuses["disabled"] else (1 if statuses["fail"] else 3)
        candidates.append((priority, row["name_sha256"], "current_only", row))
    candidates.extend((4, row["name_sha256"], "baseline_only", row)
                      for row in baseline_only)
    selected = candidates
    if len(candidates) > MAX_ROWS:
        selected = sorted(candidates, key=lambda item: (item[0], item[1]))[:MAX_ROWS]
    shown_transitions = [row for _, _, category, row in selected if category == "transitions"]
    shown_ambiguous = [row for _, _, category, row in selected if category == "ambiguous"]
    shown_current_only = [row for _, _, category, row in selected if category == "current_only"]
    shown_baseline_only = [row for _, _, category, row in selected if category == "baseline_only"]
    report["comparison"] = {
        "status": "observed",
        "baseline": {
            "input": str(baseline_path), "sha256": baseline["sha256"],
            "observation": baseline["observation"], "testcases": baseline["testcases"],
        },
        "transition_counts": transition_counts,
        "transitions": shown_transitions,
        "omitted_transitions": len(transitions) - len(shown_transitions),
        "ambiguous_duplicate_groups": shown_ambiguous,
        "omitted_ambiguous_duplicate_groups": len(ambiguous_duplicate_groups) - len(shown_ambiguous),
        "current_only_count": sum(row["count"] for row in current_only),
        "current_only_group_count": len(current_only),
        "current_only": shown_current_only,
        "omitted_current_only": len(current_only) - len(shown_current_only),
        "baseline_only_count": sum(row["count"] for row in baseline_only),
        "baseline_only_group_count": len(baseline_only),
        "baseline_only": shown_baseline_only,
        "omitted_baseline_only": len(baseline_only) - len(shown_baseline_only),
        "limitations": [
            "Unique test names are matched by the SHA-256 of the full CTest name.",
            "Same-name duplicates are compared only as status-count groups; per-case transitions are intentionally not guessed.",
            "Artifact-only comparison cannot distinguish filtering, configuration, or source changes.",
            "The shared row budget prioritizes new non-runs, failures, ambiguous duplicates, and current-only groups before recoveries.",
            "A transition is evidence to investigate, not a test-policy verdict.",
        ],
    }


def write_json_output(path: Path, report: dict) -> None:
    protected_inputs = [Path(report["input"])]
    comparison = report.get("comparison")
    if comparison and comparison.get("baseline", {}).get("input"):
        protected_inputs.append(Path(comparison["baseline"]["input"]))
    for input_path in protected_inputs:
        try:
            if path.exists() and input_path.exists() and os.path.samefile(path, input_path):
                raise ValueError("JSON output must not replace an input artifact")
        except OSError:
            pass
    try:
        existing = os.lstat(path)
        if not stat.S_ISREG(existing.st_mode):
            raise ValueError("JSON output must be a regular, non-symlink file")
    except FileNotFoundError:
        pass
    flags = os.O_WRONLY | os.O_CREAT | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as stream:
        output_stat = os.fstat(stream.fileno())
        if not stat.S_ISREG(output_stat.st_mode):
            raise ValueError("JSON output must be a regular file")
        for input_path in protected_inputs:
            try:
                input_stat = os.stat(input_path)
            except OSError:
                continue
            if (output_stat.st_dev, output_stat.st_ino) == (input_stat.st_dev, input_stat.st_ino):
                raise ValueError("JSON output must not replace an input artifact")
        stream.truncate(0)
        json.dump(report, stream, ensure_ascii=True, indent=2)
        stream.write("\n")


def cell(value) -> str:
    return html.escape(bounded_text(str(value))).replace("|", "&#124;").replace("`", "&#96;").replace("\n", " ")


def markdown(report: dict, leg: str = "") -> str:
    lines = [f"## ctest non-runs — {cell(leg)}", ""]
    if report["counts"] is not None:
        counts = report["counts"]
        lines += ["| population | count |", "| --- | --- |",
                  f"| registered (`ctest -N` raw lines) | {report['listing_raw_lines']} |",
                  f"| registered (`ctest -N` total) | {report['registered']} |",
                  f"| declared (JUnit `tests=`) | {report['declared_tests']} |",
                  f"| `<testcase>` entries | {report['testcases']} |",
                  f"| reported non-runs (`notrun`/`disabled`) | {counts['notrun'] + counts['disabled']} |", ""]
        if report["nonruns"]:
            lines += ["### Tests that did not run", "", "| test | reason | labels | output |", "| --- | --- | --- | --- |"]
            for row in report["nonruns"]:
                lines.append("| " + " | ".join(cell(row[key]) for key in ("name", "reason", "labels", "output")) + " |")
        elif report["observation"] == "observed":
            lines.append("_Every reported test ran; this is not a claim about filtered or unregistered tests._")
    comparison = report.get("comparison")
    if comparison and comparison["status"] == "observed":
        counts = comparison["transition_counts"]
        lines += ["", "### Changes from baseline artifact", "",
                  f"New failures: {counts['newly_failed']}; new non-runs: {counts['new_nonrun']}; "
                  f"failures cleared: {counts['failure_cleared']}; recovered: {counts['recovered']}; "
                  f"other status changes: {counts['changed']}; ambiguous duplicate groups: "
                  f"{counts['ambiguous_duplicate_groups']}; current-only: {comparison['current_only_count']}; "
                  f"baseline-only: {comparison['baseline_only_count']}."]
        if comparison["transitions"]:
            lines += ["", "| test | before | after | change |", "| --- | --- | --- | --- |"]
            for row in comparison["transitions"]:
                lines.append("| " + " | ".join(cell(row[key]) for key in ("name", "before", "after", "kind")) + " |")
        if comparison["ambiguous_duplicate_groups"]:
            lines += ["", "| duplicate test name | baseline statuses | current statuses |", "| --- | --- | --- |"]
            for row in comparison["ambiguous_duplicate_groups"]:
                lines.append("| " + " | ".join(cell(row[key]) for key in ("name", "before_status_counts", "after_status_counts")) + " |")
        if comparison["current_only"] or comparison["baseline_only"]:
            lines += ["", "| artifact | test name | count | statuses |", "| --- | --- | ---: | --- |"]
            for side, key in (("current only", "current_only"), ("baseline only", "baseline_only")):
                for row in comparison[key]:
                    lines.append("| " + " | ".join((cell(side), cell(row["name"]), cell(row["count"]), cell(row["status_counts"]))) + " |")
        omitted_comparison = sum(comparison[key] for key in (
            "omitted_transitions", "omitted_ambiguous_duplicate_groups",
            "omitted_current_only", "omitted_baseline_only",
        ))
        if omitted_comparison:
            lines.append(f"Omitted {omitted_comparison} comparison rows; counts cover both full artifacts.")
    lines += ["", *(f"Observation: {cell(issue)}" for issue in report["issues"])]
    if report["omitted_nonruns"] or report["omitted_issues"]:
        lines.append(f"Omitted {report['omitted_nonruns']} non-run rows and {report['omitted_issues']} issue rows; counts cover the full artifact.")
    lines += ["", "Observation only: no skip policy or current-head validation verdict."]
    return "\n".join(lines) + "\n"


def count_argument(value):
    if value == "unknown":
        return None
    if not value.isascii() or not value.isdecimal() or len(value) > 9:
        raise argparse.ArgumentTypeError("expected a nonnegative count or unknown")
    return int(value)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("junit", type=Path)
    parser.add_argument("--registered", type=count_argument)
    parser.add_argument("--raw-lines", type=count_argument)
    parser.add_argument("--leg", default="")
    parser.add_argument("--baseline", type=Path, help="compare with another CTest JUnit artifact")
    parser.add_argument("--json", action="store_true", help="emit the same observation as JSON")
    parser.add_argument("--json-output", type=Path, help="also write the observation to a regular JSON file")
    args = parser.parse_args(argv)
    report, cases = observe_with_cases(args.junit, args.registered, args.raw_lines)
    if args.baseline is not None:
        compare(report, cases, args.baseline)
    output_error = None
    if args.json_output is not None:
        try:
            write_json_output(args.json_output, report)
        except (OSError, ValueError) as error:
            output_error = bounded_text(str(error))
    print(json.dumps(report, ensure_ascii=True) if args.json else markdown(report, args.leg), end="\n" if args.json else "")
    counts = report["counts"] or {}
    print(f"ctest-nonruns: observation={report['observation']} declared={report['declared_tests']} testcases={report['testcases']} nonruns={counts.get('notrun', 0) + counts.get('disabled', 0)}", file=sys.stderr)
    if output_error is not None:
        print(f"ctest-nonruns: JSON output unavailable: {output_error}", file=sys.stderr)
    return 0 if report["observation"] == "observed" and output_error is None else 2


if __name__ == "__main__":
    raise SystemExit(main())
