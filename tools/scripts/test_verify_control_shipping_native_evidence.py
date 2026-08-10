#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/scripts/verify_control_shipping_native_evidence.py"


class NativeShippingEvidenceTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_report(
        self,
        artifact_format: str,
        *,
        platform: str = "linux",
        architectures: list[str] | None = None,
        symbol_scanner: str = "nm",
        dependency_scanner: str = "readelf",
        architecture_scanner: str = "readelf",
    ) -> None:
        artifact = self.root / f"PulpSDKSmokeProbe-{artifact_format}.bin"
        artifact.write_bytes(b"native-artifact")
        report = {
            "schema": "dev.pulp.control/shipping-report@1",
            "artifact": str(artifact),
            "artifact_size_bytes": artifact.stat().st_size,
            "profile": "production-stripped",
            "format": artifact_format,
            "platform": platform,
            "architectures": architectures or ["x86_64"],
            "symbol_scanner": symbol_scanner,
            "dependency_scanner": dependency_scanner,
            "architecture_scanner": architecture_scanner,
            "packaged_dependency_binaries_scanned": 0,
            "verdict": "pass",
        }
        path = self.root / f"PulpSDKSmokeProbe.{artifact_format}.control-shipping-report.json"
        path.write_text(json.dumps(report), encoding="utf-8")

    def run_leg(self, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "python3",
                str(SCRIPT),
                "verify-leg",
                "--report-root",
                str(self.root),
                "--leg",
                "linux-x64",
                "--platform",
                "linux",
                "--required-architectures",
                "x86_64",
                "--required-formats",
                "Standalone,VST3,CLAP,LV2",
                "--output",
                str(self.root / "linux-x64.native-shipping-leg.json"),
                *extra,
            ],
            text=True,
            capture_output=True,
        )

    @staticmethod
    def expected_formats(leg: str) -> list[str]:
        if leg == "macos-universal":
            return [
                "Standalone", "VST3", "CLAP", "LV2", "AUv2",
                "AUv3Framework", "AUv3Extension", "AUv3Host",
            ]
        return ["Standalone", "VST3", "CLAP", "LV2"]

    @staticmethod
    def aggregate_reports(
        platform: str,
        architectures: list[str],
        formats: list[str],
    ) -> list[dict[str, object]]:
        scanners = {
            "darwin": {"symbols": "nm", "dependencies": "otool", "architecture": "lipo"},
            "linux": {"symbols": "nm", "dependencies": "readelf", "architecture": "readelf"},
            "windows": {
                "symbols": "dumpbin",
                "dependencies": "dumpbin",
                "architecture": "dumpbin",
            },
        }
        return [
            {
                "format": artifact_format,
                "artifact_size_bytes": 16,
                "architectures": architectures,
                "scanners": scanners[platform],
            }
            for artifact_format in formats
        ]

    def test_real_reports_pass_and_unavailable_aax_is_explicit(self) -> None:
        for artifact_format in ("Standalone", "VST3", "CLAP", "LV2"):
            self.write_report(artifact_format)
        result = self.run_leg(
            "--aax-mode",
            "unavailable",
            "--aax-unavailable-reason",
            "aax-sdk-secret-unavailable",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        evidence = json.loads((self.root / "linux-x64.native-shipping-leg.json").read_text())
        self.assertEqual(evidence["verdict"], "pass")
        self.assertEqual(evidence["aax"], {
            "proof": False,
            "reason_code": "aax-sdk-secret-unavailable",
            "status": "unavailable",
        })
        self.assertEqual({item["format"] for item in evidence["reports"]}, {
            "Standalone", "VST3", "CLAP", "LV2"
        })

    def test_wrong_native_scanner_fails(self) -> None:
        for artifact_format in ("Standalone", "VST3", "LV2"):
            self.write_report(artifact_format)
        self.write_report("CLAP", symbol_scanner="unavailable")
        result = self.run_leg()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("used scanners", result.stderr)

    def test_byte_identical_embedded_report_copy_is_not_duplicate_proof(self) -> None:
        for artifact_format in ("Standalone", "VST3", "CLAP", "LV2"):
            self.write_report(artifact_format)
        source = self.root / "PulpSDKSmokeProbe.CLAP.control-shipping-report.json"
        embedded = self.root / "host" / source.name
        embedded.parent.mkdir()
        embedded.write_bytes(source.read_bytes())
        result = self.run_leg()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_live_artifact_fails(self) -> None:
        for artifact_format in ("Standalone", "VST3", "CLAP", "LV2"):
            self.write_report(artifact_format)
        (self.root / "PulpSDKSmokeProbe-VST3.bin").unlink()
        result = self.run_leg()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("live native artifact", result.stderr)

    def test_aggregate_accepts_only_complete_native_matrix_with_explicit_aax(self) -> None:
        evidence_root = self.root / "evidence"
        evidence_root.mkdir()
        identities = {
            "macos-universal": ("darwin", ["arm64", "x86_64"]),
            "linux-x64": ("linux", ["x86_64"]),
            "linux-arm64": ("linux", ["arm64"]),
            "windows-x64": ("windows", ["x86_64"]),
        }
        for leg, (platform, architectures) in identities.items():
            aax = {"status": "not-assessed", "proof": False}
            if leg == "macos-universal":
                aax = {
                    "status": "unavailable",
                    "proof": False,
                    "reason_code": "aax-sdk-secret-unavailable",
                }
            value = {
                "schema": "dev.pulp.control/native-shipping-leg@1",
                "leg": leg,
                "platform": platform,
                "required_architectures": architectures,
                "required_formats": self.expected_formats(leg),
                "reports": self.aggregate_reports(
                    platform, architectures, self.expected_formats(leg)
                ),
                "aax": aax,
                "verdict": "pass",
            }
            (evidence_root / f"{leg}.native-shipping-leg.json").write_text(
                json.dumps(value), encoding="utf-8"
            )
        output = self.root / "aggregate.json"
        result = subprocess.run(
            [
                "python3",
                str(SCRIPT),
                "aggregate",
                "--evidence-root",
                str(evidence_root),
                "--output",
                str(output),
            ],
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        aggregate = json.loads(output.read_text())
        self.assertTrue(aggregate["complete"])
        self.assertFalse(aggregate["aax"]["proof"])

    def test_aggregate_rejects_ambiguous_aax_skip(self) -> None:
        evidence_root = self.root / "evidence"
        evidence_root.mkdir()
        identities = {
            "macos-universal": ("darwin", ["arm64", "x86_64"]),
            "linux-x64": ("linux", ["x86_64"]),
            "linux-arm64": ("linux", ["arm64"]),
            "windows-x64": ("windows", ["x86_64"]),
        }
        for leg, (platform, architectures) in identities.items():
            value = {
                "schema": "dev.pulp.control/native-shipping-leg@1",
                "leg": leg,
                "platform": platform,
                "required_architectures": architectures,
                "required_formats": self.expected_formats(leg),
                "reports": self.aggregate_reports(
                    platform, architectures, self.expected_formats(leg)
                ),
                "aax": {"status": "not-assessed", "proof": False},
                "verdict": "pass",
            }
            (evidence_root / f"{leg}.native-shipping-leg.json").write_text(
                json.dumps(value), encoding="utf-8"
            )
        result = subprocess.run(
            [
                "python3",
                str(SCRIPT),
                "aggregate",
                "--evidence-root",
                str(evidence_root),
                "--output",
                str(self.root / "aggregate.json"),
            ],
            text=True,
            capture_output=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("AAX must be proven or explicitly unavailable", result.stderr)


if __name__ == "__main__":
    unittest.main()
