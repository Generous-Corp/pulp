#!/usr/bin/env python3

import pathlib
import tempfile
import unittest
from unittest import mock

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


CAPABILITY_DEFINITIONS = (
    'PULP_INSPECT_CAPABILITY(StateRead, "state.read", "dev.pulp.state/read@1", '
    "Sensitive, None, Background, Response, 1, 1, 1, 0)\n"
    'PULP_INSPECT_CAPABILITY(UiInput, "ui.input", "dev.pulp.ui/input@1", '
    "HighRisk, Input, HostMain, Receipt, 0, 1, 1, 0)\n"
)

CONTROL_MANIFEST = (
    "#define PULP_OPERATION(symbol, slug, input, output, result)\n"
    "#define PULP_RECEIPT_OPERATION(symbol, slug, input, output, result)\n"
    '        PULP_OPERATION(StateRead, "state/read", PULP_SCHEMA_EMPTY,\n'
    '                       PULP_SCHEMA_EMPTY, "response"),\n'
    '        PULP_RECEIPT_OPERATION(UiInput, "ui/input", PULP_SCHEMA_EMPTY,\n'
    '                               PULP_SCHEMA_EMPTY, "receipt"),\n'
)

CAPABILITY_DOC = """# Development inspector capabilities

<!-- BEGIN GENERATED capability-matrix (write) -->
| Canonical capability (legacy spelling) | `observe` | `develop` | Current reality |
|---|---:|---:|---|
| `dev.pulp.state/read@1` (`state.read`) | yes | yes | Bounded parameter snapshot |
| `dev.pulp.ui/input@1` (`ui.input`) | no | yes | Grant-controlled single event |
<!-- END GENERATED capability-matrix -->

<!-- BEGIN GENERATED operation-matrix (write) -->
| Typed operation | Gating capability (legacy spelling) | Result |
|---|---|---|
| `dev.pulp.state/read@1` | `dev.pulp.state/read@1` (`state.read`) | `response` |
| `dev.pulp.ui/input@1` | `dev.pulp.ui/input@1` (`ui.input`) | `receipt` |
<!-- END GENERATED operation-matrix -->
"""


class ControlOperationDocumentationTests(unittest.TestCase):
    """The operation hop: a capability row alone must not license an operation."""

    def setUp(self) -> None:
        self.definitions = inspector_truth_check.parse_capability_definitions(
            CAPABILITY_DEFINITIONS
        )
        self.operations = inspector_truth_check.parse_control_operations(CONTROL_MANIFEST)

    def test_parses_both_sides(self) -> None:
        self.assertEqual(
            [definition.legacy_id for definition in self.definitions],
            ["state.read", "ui.input"],
        )
        self.assertEqual(
            [operation.operation_id for operation in self.operations],
            ["dev.pulp.state/read@1", "dev.pulp.ui/input@1"],
        )
        self.assertEqual(
            [operation.result_kind for operation in self.operations],
            ["response", "receipt"],
        )

    def test_scan_is_bounded_to_the_registry_array(self) -> None:
        # A plain operation's result kind is the last quoted token before the
        # macro's close, so an unbounded tail scan would read one out of the
        # code below the array.
        bounded = (
            "constexpr auto kControlOperations =\n"
            "    std::to_array<ControlOperationDescriptor>({\n"
            '        PULP_OPERATION(StateRead, "state/read", PULP_SCHEMA_EMPTY,\n'
            '                       PULP_SCHEMA_EMPTY, "response"),\n'
            "    });\n"
            'std::string_view later_helper() { return cache_lookup("receipt"); }\n'
        )
        operations = inspector_truth_check.parse_control_operations(bounded)
        self.assertEqual([operation.result_kind for operation in operations], ["response"])

    def test_macro_definitions_are_not_read_as_operations(self) -> None:
        self.assertEqual(
            inspector_truth_check.parse_control_operations(
                "#define PULP_OPERATION(symbol, slug, input, output, result)\n"
            ),
            [],
        )

    def test_accepts_documented_operations(self) -> None:
        self.assertEqual(
            inspector_truth_check.operation_doc_errors(
                self.definitions, self.operations, CAPABILITY_DOC
            ),
            [],
        )

    def test_rejects_new_operation_on_a_documented_capability(self) -> None:
        manifest = CONTROL_MANIFEST + (
            '        PULP_OPERATION(StateRead, "state/read.extended", PULP_SCHEMA_EMPTY,\n'
            '                       PULP_SCHEMA_EMPTY, "response"),\n'
        )
        operations = inspector_truth_check.parse_control_operations(manifest)
        errors = inspector_truth_check.operation_doc_errors(
            self.definitions, operations, CAPABILITY_DOC
        )
        self.assertTrue(
            any(
                "omit operation `dev.pulp.state/read.extended@1`" in error
                for error in errors
            ),
            errors,
        )

    def test_rejects_operation_bound_to_the_wrong_capability(self) -> None:
        doc = CAPABILITY_DOC.replace(
            "| `dev.pulp.ui/input@1` | `dev.pulp.ui/input@1` (`ui.input`) | `receipt` |",
            "| `dev.pulp.ui/input@1` | `dev.pulp.state/read@1` (`state.read`) | `receipt` |",
        )
        errors = inspector_truth_check.operation_doc_errors(
            self.definitions, self.operations, doc
        )
        self.assertTrue(any("the registry gates it on" in error for error in errors), errors)

    def test_rejects_stale_result_kind(self) -> None:
        doc = CAPABILITY_DOC.replace("(`ui.input`) | `receipt` |", "(`ui.input`) | `response` |")
        errors = inspector_truth_check.operation_doc_errors(
            self.definitions, self.operations, doc
        )
        self.assertTrue(any("record result `response`" in error for error in errors), errors)

    def test_rejects_retired_operation_left_in_the_docs(self) -> None:
        doc = CAPABILITY_DOC.replace(
            "<!-- END GENERATED operation-matrix -->",
            "| `dev.pulp.ghost/read@1` | `dev.pulp.state/read@1` (`state.read`) | `response` |\n"
            "<!-- END GENERATED operation-matrix -->",
        )
        errors = inspector_truth_check.operation_doc_errors(
            self.definitions, self.operations, doc
        )
        self.assertTrue(any("unknown operation" in error for error in errors), errors)

    def test_reports_a_dead_parser_rather_than_passing(self) -> None:
        errors = inspector_truth_check.operation_doc_errors(
            self.definitions, [], CAPABILITY_DOC
        )
        self.assertTrue(any("parsed zero operations" in error for error in errors), errors)


class UndocumentedRealityTests(unittest.TestCase):
    """The generator's placeholder is not documentation."""

    def setUp(self) -> None:
        self.definitions = inspector_truth_check.parse_capability_definitions(
            CAPABILITY_DEFINITIONS
        )

    def test_accepts_hand_written_reality(self) -> None:
        self.assertEqual(
            inspector_truth_check.undocumented_reality_errors(
                self.definitions, CAPABILITY_DOC
            ),
            [],
        )

    def test_rejects_the_generated_placeholder(self) -> None:
        doc = CAPABILITY_DOC.replace(
            "Grant-controlled single event", inspector_truth_check.UNDOCUMENTED_REALITY
        )
        errors = inspector_truth_check.undocumented_reality_errors(
            self.definitions, doc
        )
        self.assertTrue(
            any("current reality of `ui.input` undocumented" in error for error in errors),
            errors,
        )
        self.assertFalse(any("state.read" in error for error in errors), errors)

    def test_rejects_an_emptied_reality_cell(self) -> None:
        # Deleting the placeholder documents nothing, and every other rule in
        # the file still accepts the row, so emptiness has to be rejected here
        # or it becomes the way around this gate.
        doc = CAPABILITY_DOC.replace("Grant-controlled single event", "")
        errors = inspector_truth_check.undocumented_reality_errors(
            self.definitions, doc
        )
        self.assertTrue(
            any("current reality of `ui.input` undocumented" in error for error in errors),
            errors,
        )

    def test_rejects_a_whitespace_only_reality_cell(self) -> None:
        doc = CAPABILITY_DOC.replace("Grant-controlled single event", "   ")
        errors = inspector_truth_check.undocumented_reality_errors(
            self.definitions, doc
        )
        self.assertTrue(
            any("current reality of `ui.input` undocumented" in error for error in errors),
            errors,
        )

    def test_check_root_runs_the_undocumented_reality_gate(self) -> None:
        root = pathlib.Path(inspector_truth_check.__file__).resolve().parents[2]
        sentinel = "undocumented reality gate reached"
        with mock.patch.object(
            inspector_truth_check, "undocumented_reality_errors", return_value=[sentinel]
        ) as gate:
            errors = inspector_truth_check.check_root(root)
        self.assertTrue(gate.called)
        self.assertIn(sentinel, errors)


class GeneratedMatrixWriteTests(unittest.TestCase):
    """`--write` must emit exactly what `--check` accepts, and preserve prose."""

    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tempdir.name)
        for relative_path, text in (
            (inspector_truth_check.CAPABILITY_DEFINITIONS_PATH, CAPABILITY_DEFINITIONS),
            (inspector_truth_check.CONTROL_MANIFEST_PATH, CONTROL_MANIFEST),
            (inspector_truth_check.CAPABILITY_DOC_PATH, CAPABILITY_DOC),
        ):
            path = self.root / relative_path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def _doc(self) -> str:
        return (self.root / inspector_truth_check.CAPABILITY_DOC_PATH).read_text(
            encoding="utf-8"
        )

    def _check(self) -> list[str]:
        definitions = inspector_truth_check.parse_capability_definitions(
            CAPABILITY_DEFINITIONS
        )
        operations = inspector_truth_check.parse_control_operations(CONTROL_MANIFEST)
        return inspector_truth_check.operation_doc_errors(
            definitions, operations, self._doc()
        )

    def test_write_is_idempotent_on_matching_docs(self) -> None:
        self.assertFalse(inspector_truth_check.write_root(self.root))
        self.assertEqual(self._doc(), CAPABILITY_DOC)

    def test_write_restores_a_drifted_operation_matrix(self) -> None:
        path = self.root / inspector_truth_check.CAPABILITY_DOC_PATH
        path.write_text(
            CAPABILITY_DOC.replace(
                "| `dev.pulp.ui/input@1` | `dev.pulp.ui/input@1` (`ui.input`) | `receipt` |\n",
                "",
            ),
            encoding="utf-8",
        )
        self.assertTrue(self._check())
        self.assertTrue(inspector_truth_check.write_root(self.root))
        self.assertEqual(self._check(), [])

    def test_write_preserves_hand_written_reality_prose(self) -> None:
        path = self.root / inspector_truth_check.CAPABILITY_DOC_PATH
        path.write_text(
            CAPABILITY_DOC.replace("| yes | yes |", "| no | no |"), encoding="utf-8"
        )
        inspector_truth_check.write_root(self.root)
        doc = self._doc()
        self.assertIn(
            "| `dev.pulp.state/read@1` (`state.read`) | yes | yes | "
            "Bounded parameter snapshot |",
            doc,
        )
        self.assertNotIn(inspector_truth_check.UNDOCUMENTED_REALITY, doc)

    def test_write_marks_an_undocumented_new_capability(self) -> None:
        path = self.root / inspector_truth_check.CAPABILITY_DEFINITIONS_PATH
        path.write_text(
            CAPABILITY_DEFINITIONS
            + 'PULP_INSPECT_CAPABILITY(LogsRead, "logs.read", "dev.pulp.logs/read@1", '
            "Sensitive, None, Protocol, Response, 1, 1, 1, 0)\n",
            encoding="utf-8",
        )
        inspector_truth_check.write_root(self.root)
        self.assertIn(inspector_truth_check.UNDOCUMENTED_REALITY, self._doc())

    def test_written_placeholder_does_not_pass_the_check(self) -> None:
        definitions = (
            CAPABILITY_DEFINITIONS
            + 'PULP_INSPECT_CAPABILITY(LogsRead, "logs.read", "dev.pulp.logs/read@1", '
            "Sensitive, None, Protocol, Response, 1, 1, 1, 0)\n"
        )
        path = self.root / inspector_truth_check.CAPABILITY_DEFINITIONS_PATH
        path.write_text(definitions, encoding="utf-8")
        inspector_truth_check.write_root(self.root)
        errors = inspector_truth_check.undocumented_reality_errors(
            inspector_truth_check.parse_capability_definitions(definitions), self._doc()
        )
        self.assertTrue(
            any("current reality of `logs.read` undocumented" in error for error in errors),
            errors,
        )

    def test_write_refuses_a_document_without_generated_regions(self) -> None:
        path = self.root / inspector_truth_check.CAPABILITY_DOC_PATH
        path.write_text("# no generated regions here\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            inspector_truth_check.write_root(self.root)

    def test_write_refuses_an_empty_registry(self) -> None:
        path = self.root / inspector_truth_check.CAPABILITY_DEFINITIONS_PATH
        path.write_text("// nothing here\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            inspector_truth_check.write_root(self.root)


class OperationGateWiringTests(unittest.TestCase):
    """Covering the rule is not covering the gate that runs it."""

    def test_check_root_runs_the_operation_documentation_gate(self) -> None:
        root = pathlib.Path(inspector_truth_check.__file__).resolve().parents[2]
        sentinel = "operation documentation gate reached"
        with mock.patch.object(
            inspector_truth_check, "operation_doc_errors", return_value=[sentinel]
        ) as gate:
            errors = inspector_truth_check.check_root(root)
        self.assertTrue(gate.called)
        self.assertIn(sentinel, errors)


if __name__ == "__main__":
    unittest.main()
