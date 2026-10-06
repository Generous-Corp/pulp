#!/usr/bin/env python3
"""Tests for the fail-closed CDP browser fidelity receipt contract."""

from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path

import verify_browser_fidelity_receipt as verifier


SHA = "2301ab903a561f6d460e588070a31f193ec2a8355dea463cfd7edc217949d968"
TITLE = "Spectr — zoomable filter bank"


def receipt(source: Path) -> dict:
    def run() -> dict:
        return {
            "consoleErrors": [],
            "dom": {"title": TITLE, "ready": 1, "bodyText": "SPECTR", "rootChildren": 1},
            "marker": {"count": 1, "text": ""},
            "networkFailures": [],
            "screenshotSha256": SHA,
        }

    source_bytes = source.read_bytes()
    source_sha256 = hashlib.sha256(source_bytes).hexdigest()
    mutated_source_sha256 = hashlib.sha256(
        source_bytes.replace(b"ReactDOM.createRoot", b"ReactDOM.brokenRoot")
    ).hexdigest()
    return {
        "source": str(source),
        "sourceSha256": source_sha256,
        "positive": run(),
        "repeat": run(),
        "deterministicDomMarker": True,
        "negativeControl": {
            "mutation": {
                "kind": "broken-editor-source",
                "sourceSha256": source_sha256,
                "mutatedSourceSha256": mutated_source_sha256,
            },
            "outcome": {
                "status": "rejected",
                "errorCode": "capture-mount-missing",
            },
        },
    }


class BrowserFidelityReceiptTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / "editor.html"
        self.source.write_text(
            "<!doctype html><div id='root'>SPECTR</div>\n"
            "<script>ReactDOM.createRoot(root)</script>\n",
            encoding="utf-8",
        )
        self.path = self.root / "receipt.json"
        self.payload = receipt(self.source)
        self.path.write_text(json.dumps(self.payload), encoding="utf-8")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_current_receipt_passes_and_returns_stable_summary(self) -> None:
        summary = verifier.validate_receipt(
            self.payload,
            expected_screenshot_sha256=SHA,
            expected_title=TITLE,
        )
        self.assertEqual(summary["source"], str(self.source.resolve()))
        self.assertEqual(summary["marker_count"], 1)
        self.assertEqual(summary["root_children"], 1)
        self.assertEqual(summary["screenshot_sha256"], SHA)

    def test_copied_receipt_can_validate_current_source_override(self) -> None:
        copied = copy.deepcopy(self.payload)
        copied["source"] = str(self.root / "old-checkout" / "editor.html")
        summary = verifier.validate_receipt(copied, source_override=self.source)
        self.assertEqual(summary["source"], str(self.source.resolve()))

    def test_negative_changed_source_bytes_are_rejected(self) -> None:
        self.source.write_text("<!doctype html><div id='root'>CHANGED</div>\n", encoding="utf-8")
        with self.assertRaisesRegex(verifier.ReceiptError, "sourceSha256"):
            verifier.validate_receipt(self.payload)

    def test_negative_source_digest_shape_is_rejected(self) -> None:
        broken = copy.deepcopy(self.payload)
        broken["sourceSha256"] = "not-a-digest"
        with self.assertRaisesRegex(verifier.ReceiptError, "sourceSha256"):
            verifier.validate_receipt(broken)

    def test_cli_positive_emits_json_summary(self) -> None:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = verifier.main([str(self.path), "--json", "--expected-title", TITLE])
        self.assertEqual(rc, 0)
        self.assertEqual(json.loads(out.getvalue())["screenshot_sha256"], SHA)

    def test_negative_console_error_is_rejected(self) -> None:
        broken = copy.deepcopy(self.payload)
        broken["positive"]["consoleErrors"] = ["uncaught"]
        with self.assertRaisesRegex(verifier.ReceiptError, "consoleErrors"):
            verifier.validate_receipt(broken)

    def test_negative_duplicate_marker_is_rejected(self) -> None:
        broken = copy.deepcopy(self.payload)
        broken["repeat"]["marker"]["count"] = 2
        with self.assertRaisesRegex(verifier.ReceiptError, "marker.count"):
            verifier.validate_receipt(broken)

    def test_negative_boolean_marker_count_is_rejected(self) -> None:
        broken = copy.deepcopy(self.payload)
        broken["positive"]["marker"]["count"] = True
        with self.assertRaisesRegex(verifier.ReceiptError, "marker.count"):
            verifier.validate_receipt(broken)

    def test_negative_repeat_screenshot_mismatch_is_rejected(self) -> None:
        broken = copy.deepcopy(self.payload)
        broken["repeat"]["screenshotSha256"] = "0" * 64
        with self.assertRaisesRegex(verifier.ReceiptError, "screenshot SHA-256"):
            verifier.validate_receipt(broken)

    def test_negative_missing_root_is_rejected(self) -> None:
        broken = copy.deepcopy(self.payload)
        del broken["positive"]["dom"]["rootChildren"]
        with self.assertRaisesRegex(verifier.ReceiptError, "rootChildren"):
            verifier.validate_receipt(broken)

    def test_negative_cli_missing_source_is_error(self) -> None:
        missing = self.root / "missing.json"
        rc = verifier.main([str(missing)])
        self.assertEqual(rc, 1)

    def test_negative_forged_rejection_without_source_binding_is_rejected(self) -> None:
        broken = copy.deepcopy(self.payload)
        broken["negativeControl"] = {"outcome": {"status": "rejected"}}
        with self.assertRaisesRegex(verifier.ReceiptError, "negativeControl"):
            verifier.validate_receipt(broken)

    def test_negative_mutation_bound_evidence_passes(self) -> None:
        summary = verifier.validate_receipt(self.payload)
        self.assertEqual(
            summary["negative_control"]["mutation"]["sourceSha256"],
            self.payload["sourceSha256"],
        )

    def test_negative_forged_mutation_digest_is_rejected(self) -> None:
        broken = copy.deepcopy(self.payload)
        broken["negativeControl"]["mutation"]["mutatedSourceSha256"] = "1" * 64
        with self.assertRaisesRegex(verifier.ReceiptError, "canonical source mutation"):
            verifier.validate_receipt(broken)

    def test_negative_mutation_without_exact_source_match_is_rejected(self) -> None:
        broken = copy.deepcopy(self.payload)
        self.source.write_text("<!doctype html><div id='root'>SPECTR</div>\n",
                               encoding="utf-8")
        source_sha256 = hashlib.sha256(self.source.read_bytes()).hexdigest()
        broken["sourceSha256"] = source_sha256
        broken["negativeControl"]["mutation"]["sourceSha256"] = source_sha256
        broken["negativeControl"]["mutation"]["mutatedSourceSha256"] = "1" * 64
        with self.assertRaisesRegex(verifier.ReceiptError, "did not match"):
            verifier.validate_receipt(broken)


if __name__ == "__main__":
    unittest.main()
