#!/usr/bin/env python3

import pathlib
import tempfile
import unittest

import inspector_truth_check


class InspectorAuthorityDeletionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tempdir.name)
        (self.root / "inspect/include/pulp/inspect").mkdir(parents=True)
        (self.root / "inspect/include/pulp/inspect/client.hpp").write_text(
            "struct InspectorClientTarget {};\n", encoding="utf-8"
        )
        (self.root / "inspect").mkdir(exist_ok=True)
        (self.root / "inspect/CMakeLists.txt").write_text(
            "add_library(pulp-inspect-client src/control_inspector_client.cpp)\n",
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def test_accepts_deleted_authority(self) -> None:
        self.assertEqual(
            inspector_truth_check.security_implementation_errors(self.root), []
        )

    def test_rejects_restored_authority_file(self) -> None:
        path = self.root / "inspect/src/inspector_server.cpp"
        path.parent.mkdir(parents=True)
        path.write_text("legacy listener\n", encoding="utf-8")
        errors = inspector_truth_check.security_implementation_errors(self.root)
        self.assertTrue(any("inspector_server.cpp" in error for error in errors))

    def test_rejects_restored_raw_client_api(self) -> None:
        path = self.root / "inspect/include/pulp/inspect/client.hpp"
        path.write_text("class InspectorClient {};\n", encoding="utf-8")
        errors = inspector_truth_check.security_implementation_errors(self.root)
        self.assertTrue(any("raw Inspector client" in error for error in errors))

    def test_rejects_restored_authority_target(self) -> None:
        path = self.root / "inspect/CMakeLists.txt"
        path.write_text("add_library(pulp-inspect-discovery x.cpp)\n", encoding="utf-8")
        errors = inspector_truth_check.security_implementation_errors(self.root)
        self.assertTrue(any("authority target" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
