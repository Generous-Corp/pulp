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

    def test_rejects_restored_motion_command(self) -> None:
        path = self.root / "experimental/pulp-rs/src/cmd/motion.rs"
        path.parent.mkdir(parents=True)
        path.write_text("retired command\n", encoding="utf-8")
        errors = inspector_truth_check.security_implementation_errors(self.root)
        self.assertTrue(any("motion.rs" in error for error in errors))


class ReducedPublicSurfaceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tempdir.name)
        (self.root / "tools/cli").mkdir(parents=True)
        (self.root / "tools/mcp").mkdir(parents=True)
        (self.root / "experimental/pulp-rs/src/cmd").mkdir(parents=True)
        (self.root / "tools/cli/cmd_inspect.cpp").write_text(
            '"pulp inspect profiles [--json]"\n'
            '"pulp inspect audit ARTIFACT [--json]"\n',
            encoding="utf-8",
        )
        (self.root / "tools/mcp/pulp_mcp.cpp").write_text(
            '"name":"pulp_inspect_profiles"\n'
            '"name":"pulp_trace_start"\n'
            '"name":"pulp_trace_stop"\n',
            encoding="utf-8",
        )
        (self.root / "experimental/pulp-rs/src/cmd/trace_dispatch.rs").write_text(
            "if let Sub::Query(q) { return run_offline_query(q); }\n"
            "if matches!(sub, Sub::Start(_) | Sub::Stop(_)) {\n"
            "  let call = to_control_call(sub);\n"
            "}\n",
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def test_accepts_reduced_surface(self) -> None:
        self.assertEqual(inspector_truth_check.public_surface_errors(self.root), [])

    def test_rejects_restored_live_inspect_tool(self) -> None:
        path = self.root / "tools/mcp/pulp_mcp.cpp"
        path.write_text(path.read_text() + '"name":"pulp_inspect_list"\n')
        errors = inspector_truth_check.public_surface_errors(self.root)
        self.assertTrue(any("pulp_inspect_list" in error for error in errors))

    def test_rejects_restored_live_inspect_verb(self) -> None:
        path = self.root / "tools/cli/cmd_inspect.cpp"
        path.write_text(path.read_text() + 'if (verb == "list") {}\n')
        errors = inspector_truth_check.public_surface_errors(self.root)
        self.assertTrue(any("retired live route: list" in error for error in errors))

    def test_rejects_restored_motion_tool(self) -> None:
        path = self.root / "tools/mcp/pulp_mcp.cpp"
        path.write_text(path.read_text() + '"name":"pulp_motion_record"\n')
        errors = inspector_truth_check.public_surface_errors(self.root)
        self.assertTrue(any("pulp_motion_" in error for error in errors))

    def test_rejects_missing_canonical_trace_contract(self) -> None:
        (self.root / "experimental/pulp-rs/src/cmd/trace_dispatch.rs").write_text("")
        errors = inspector_truth_check.public_surface_errors(self.root)
        self.assertTrue(any("canonical lifecycle control" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
