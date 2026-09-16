#!/usr/bin/env python3
"""Negative controls for the bounded DSP provenance audit."""

from __future__ import annotations

import importlib.util
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("dsp_provenance_audit.py")
SPEC = importlib.util.spec_from_file_location("dsp_provenance_audit", MODULE_PATH)
assert SPEC and SPEC.loader
AUDIT = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = AUDIT
SPEC.loader.exec_module(AUDIT)
REPO_ROOT = Path(__file__).resolve().parents[2]


class DspProvenanceAuditTests(unittest.TestCase):
    def fixture(self) -> Path:
        owner = tempfile.TemporaryDirectory()
        self.addCleanup(owner.cleanup)
        temporary = Path(owner.name)
        paths = set(AUDIT.AUDITED_TEXT_PATHS)
        paths.update(
            {
                Path("DEPENDENCIES.md"),
                Path("NOTICE.md"),
                Path("docs/guides/cmajor.md"),
                Path("tools/scripts/cmajor_external.py"),
            }
        )
        for stem, reference in AUDIT.REFERENCE_HEADERS.items():
            paths.add(reference.parent / f"generated_{stem}.hpp")
            paths.add(reference.parent / f"faust_{stem}.hpp")
            paths.add(reference.parent / f"{stem}.dsp")
        for relative in paths:
            target = temporary / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO_ROOT / relative, target)
        return temporary

    def test_repository_contract_passes(self) -> None:
        self.assertEqual(AUDIT.audit(REPO_ROOT), [])

    def test_generated_source_claim_is_rejected(self) -> None:
        root = self.fixture()
        self.assertEqual(AUDIT.audit(root), [])
        path = root / "docs/guides/examples.md"
        path.write_text(path.read_text() + "\nchecked-in generated DSP\n")
        self.assertTrue(any("generation claim" in error for error in AUDIT.audit(root)))

    def test_authoritative_module_claim_is_rejected(self) -> None:
        root = self.fixture()
        self.assertEqual(AUDIT.audit(root), [])
        path = root / "docs/reference/modules.md"
        path.write_text(path.read_text() + "\nOffline code generation into checked-in C++ headers\n")
        self.assertTrue(any("generation claim" in error for error in AUDIT.audit(root)))

    def test_false_redistribution_notice_is_rejected(self) -> None:
        root = self.fixture()
        self.assertEqual(AUDIT.audit(root), [])
        path = root / "NOTICE.md"
        path.write_text(path.read_text() + "\n## Faust\n")
        self.assertTrue(any("redistributed-tool" in error for error in AUDIT.audit(root)))

    def test_broken_forwarding_header_is_rejected(self) -> None:
        root = self.fixture()
        self.assertEqual(AUDIT.audit(root), [])
        (root / "examples/faust-gain/generated_gain.hpp").write_text("#pragma once\n")
        self.assertTrue(any("forwarding contract" in error for error in AUDIT.audit(root)))

    def test_required_faust_dependency_is_rejected(self) -> None:
        root = self.fixture()
        self.assertEqual(AUDIT.audit(root), [])
        path = root / "tools/cmake/PulpFaust.cmake"
        path.write_text(path.read_text() + '\nmessage(FATAL_ERROR "Faust required")\n')
        self.assertTrue(any("externally supplied" in error for error in AUDIT.audit(root)))


if __name__ == "__main__":
    unittest.main()
