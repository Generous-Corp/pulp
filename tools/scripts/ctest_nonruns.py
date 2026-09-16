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


def observe(path: Path, registered=None, raw_lines=None) -> dict:
    report = {
        "schema": "pulp.ctest-nonruns.v1", "observation": "unavailable",
        "input": str(path), "sha256": None, "bytes": None,
        "registered": registered, "listing_raw_lines": raw_lines,
        "declared_tests": None, "testcases": None, "counts": None,
        "nonruns": [], "omitted_nonruns": 0, "issues": [],
        "limitations": [
            "Describes only the supplied artifact, not current-head or loaded-binary proof.",
            "Registered count is caller-supplied context, not a selection or provenance check.",
            "Filtered and configure-time absent tests are not identifiable from this report.",
            "Observer exit status is not the CTest exit status or a skip-policy verdict.",
        ],
    }
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
        rows = []
        for index, case in enumerate(cases):
            status = case.get("status", "")
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
    return report


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
    parser.add_argument("--json", action="store_true", help="emit the same observation as JSON")
    args = parser.parse_args(argv)
    report = observe(args.junit, args.registered, args.raw_lines)
    print(json.dumps(report, ensure_ascii=True) if args.json else markdown(report, args.leg), end="\n" if args.json else "")
    counts = report["counts"] or {}
    print(f"ctest-nonruns: observation={report['observation']} declared={report['declared_tests']} testcases={report['testcases']} nonruns={counts.get('notrun', 0) + counts.get('disabled', 0)}", file=sys.stderr)
    return 0 if report["observation"] == "observed" else 2


if __name__ == "__main__":
    raise SystemExit(main())
