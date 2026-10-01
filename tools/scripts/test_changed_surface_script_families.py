#!/usr/bin/env python3
"""Contract for the generated changed-surface script families."""

from __future__ import annotations

import json
import subprocess
import tempfile
import tomllib
import unittest
from pathlib import Path

import changed_surface_inventory as inventory
import changed_surface_script_families as families


BUILD = "/b"


def ctest(name: str, *command: str, labels: list[str] | None = None,
          fixtures: list[str] | None = None) -> dict:
    properties = [{"name": "WORKING_DIRECTORY", "value": "/repo"}]
    if labels:
        properties.append({"name": "LABELS", "value": labels})
    if fixtures:
        properties.append({"name": "FIXTURES_REQUIRED", "value": fixtures})
    return {"name": name, "command": list(command or ["python3", "x.py"]), "properties": properties}


def model(*targets: tuple[str, str]) -> inventory.CodeModel:
    return inventory.CodeModel(
        targets={
            name: inventory.Target(name=name, type="EXECUTABLE", source_dir=".", build_dir=".",
                                   sources=[], artifacts=[artifact])
            for name, artifact in targets
        },
        source_root="/repo",
        build_root=BUILD,
    )


class FamilyFixture(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.declared: dict[str, dict] = {}
        self.tests: list[dict] = []
        self.write(".agents/skills/ci/SKILL.md", "# ci\n")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def write(self, rel: str, body: str = "") -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")

    def script_test(self, name: str, entry: str, inputs: list[str] | None = None,
                    labels: list[str] | None = None, command: list[str] | None = None,
                    fixtures: list[str] | None = None) -> None:
        self.declared[name] = {"entry": entry, "inputs": sorted(set((inputs or []) + [entry])),
                               "kind": "python"}
        self.tests.append(ctest(name, *(command or ["python3", entry]), labels=labels,
                                fixtures=fixtures))

    def generate(self, *targets: tuple[str, str]) -> dict[str, dict]:
        self.write("test/ctest_script_inputs.json",
                   json.dumps({"schema": "pulp-ctest-script-inputs/v1", "tests": self.declared,
                               "executables": {}}))
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)
        generated = families.generate(self.root, self.tests, model(*targets))
        return {family["name"]: family for family in generated}

    def family_for(self, generated: dict[str, dict], path: str) -> list[str]:
        return sorted(name for name, family in generated.items() if path in family["paths"])

    def add_whole_tree(self) -> None:
        self.write("tools/scripts/docs_noise_lint.py", "")
        self.script_test("docs-noise-lint", "tools/scripts/docs_noise_lint.py")



class GeneratedFamiliesTest(FamilyFixture):
    def test_script_maps_to_exactly_its_readers_and_the_whole_tree_family(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/helper.py", "")
        self.write("tools/scripts/test_helper.py", "import helper\n")
        self.script_test("helper-selftest", "tools/scripts/test_helper.py", ["tools/scripts/helper.py"])
        generated = self.generate()
        owners = self.family_for(generated, "tools/scripts/helper.py")
        self.assertEqual(len(owners), 2)
        reader_family = generated[next(n for n in owners if n != "script-surface-whole-tree")]
        self.assertEqual(reader_family["tests"], ["helper-selftest"])
        self.assertEqual(reader_family["build_targets"], ["pulp-cli"])
        whole_tree = generated["script-surface-whole-tree"]
        self.assertIn("tools/scripts/helper.py", whole_tree["paths"])
        self.assertIn("tools/scripts/test_helper.py", whole_tree["paths"])
        self.assertEqual(whole_tree["tests"], ["docs-noise-lint"])

    def test_script_native_code_can_run_stays_unmapped_with_what_it_imports(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/runner.py", "import shared\n")
        self.write("tools/scripts/shared.py", "")
        self.write("tools/cli/cmd_run.cpp", 'auto s = root / "tools" / "scripts" / "runner.py";\n')
        self.write("tools/scripts/test_runner.py", "")
        self.script_test("runner-selftest", "tools/scripts/test_runner.py",
                         ["tools/scripts/runner.py", "tools/scripts/shared.py"])
        generated = self.generate()
        self.assertEqual(self.family_for(generated, "tools/scripts/runner.py"), [])
        self.assertEqual(self.family_for(generated, "tools/scripts/shared.py"), [])
        self.assertNotEqual(self.family_for(generated, "tools/scripts/test_runner.py"), [])

    def test_cmake_may_name_a_script_only_as_a_declared_test_entry(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/test_entry.py", "")
        self.write("tools/scripts/codegen.py", "")
        self.write("test/cmake/tests.cmake",
                   "add_test(NAME entry COMMAND python3 tools/scripts/test_entry.py)\n"
                   "add_custom_command(COMMAND python3 tools/scripts/codegen.py)\n")
        self.script_test("entry", "tools/scripts/test_entry.py", ["tools/scripts/codegen.py"])
        generated = self.generate()
        self.assertNotEqual(self.family_for(generated, "tools/scripts/test_entry.py"), [])
        self.assertEqual(self.family_for(generated, "tools/scripts/codegen.py"), [])

    def test_reader_running_a_build_product_selects_its_producer_target(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/test_tool.py", "")
        self.script_test("tool-selftest", "tools/scripts/test_tool.py",
                         command=["python3", "tools/scripts/test_tool.py", f"--tool={BUILD}/bin/tool"])
        generated = self.generate(("pulp-tool", f"{BUILD}/bin/tool"))
        owners = [n for n in self.family_for(generated, "tools/scripts/test_tool.py")
                  if n != "script-surface-whole-tree"]
        self.assertEqual(len(owners), 1)
        self.assertEqual(generated[owners[0]]["build_targets"], ["pulp-cli", "pulp-tool"])

    def test_reader_running_an_example_product_blocks_in_every_configuration(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/test_demo.py", "")
        self.script_test("demo-selftest", "tools/scripts/test_demo.py",
                         command=["python3", "tools/scripts/test_demo.py", f"{BUILD}/examples/demo/demo"])
        examples_off = self.generate()
        self.assertEqual(self.family_for(examples_off, "tools/scripts/test_demo.py"), [])
        demo = inventory.Target(name="demo", type="EXECUTABLE", source_dir="examples/demo",
                                build_dir=".", sources=[], artifacts=[f"{BUILD}/examples/demo/demo"])
        examples_on = model()
        examples_on.targets["demo"] = demo
        generated = families.generate(self.root, self.tests, examples_on)
        self.assertEqual(
            [f for f in generated if "tools/scripts/test_demo.py" in f["paths"]], [])

    def test_reader_needing_a_fixture_blocks_the_mapping(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/test_fixture.py", "")
        self.script_test("fixture-selftest", "tools/scripts/test_fixture.py", fixtures=["setup"])
        generated = self.generate()
        self.assertEqual(self.family_for(generated, "tools/scripts/test_fixture.py"), [])

    def test_reader_outside_the_authoritative_corpus_is_not_selected(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/shared_util.py", "")
        self.write("tools/scripts/test_slow.py", "")
        self.write("tools/scripts/test_fast.py", "")
        self.script_test("slow-selftest", "tools/scripts/test_slow.py",
                         ["tools/scripts/shared_util.py"], labels=["slow"])
        self.script_test("fast-selftest", "tools/scripts/test_fast.py", ["tools/scripts/shared_util.py"])
        generated = self.generate()
        # Only slow readers: nothing in the full suite reads it either, so it has no family.
        self.assertEqual(self.family_for(generated, "tools/scripts/test_slow.py"), [])
        owners = [n for n in self.family_for(generated, "tools/scripts/shared_util.py")
                  if n != "script-surface-whole-tree"]
        self.assertEqual([generated[n]["tests"] for n in owners], [["fast-selftest"]])

    def test_skill_docs_select_the_tests_that_read_the_skills_tree(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/skills_doc_check.py", 'ROOT = ".agents/skills"\n')
        self.script_test("skills-doc-sync", "tools/scripts/skills_doc_check.py")
        self.write("tools/scripts/test_other.py", "")
        self.script_test("other-selftest", "tools/scripts/test_other.py")
        generated = self.generate()
        skills = generated["agent-skill-docs"]
        self.assertEqual(skills["paths"], [".agents/skills/*/SKILL.md"])
        self.assertEqual(skills["tests"], ["skills-doc-sync"])
        self.assertIn(".agents/skills/*/SKILL.md", generated["script-surface-whole-tree"]["paths"])

    def test_no_whole_tree_test_refuses_to_bound_anything(self) -> None:
        self.write("tools/scripts/test_alone.py", "")
        self.script_test("alone-selftest", "tools/scripts/test_alone.py")
        with self.assertRaises(families.GenerationError):
            self.generate()

    def test_rendered_block_is_valid_policy_toml_and_splices_idempotently(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/test_a.py", "")
        self.script_test("a-selftest", "tools/scripts/test_a.py")
        block = families.render(list(self.generate().values()))
        config = ("[targets.mac.changed_surface_selection]\nschema_version = 3\n"
                  f"{families.BEGIN}\nstale\n{families.END}\n"
                  "[targets.mac.changed_surface_selection.execution]\nmode = \"shadow\"\n")
        spliced = families.splice(config, block)
        self.assertEqual(families.splice(spliced, block), spliced)
        self.assertNotIn("stale", spliced)
        parsed = tomllib.loads(spliced)["targets"]["mac"]["changed_surface_selection"]
        self.assertEqual({f["name"] for f in parsed["families"]},
                         {"script-surface-whole-tree"} | {n for n in self.generate() if n != "script-surface-whole-tree"})
        self.assertEqual(parsed["execution"]["mode"], "shadow")
        with self.assertRaises(families.GenerationError):
            families.splice("no markers\n", block)


class DriftCheckTest(FamilyFixture):
    """`--check` blocks only a change a stale family could bound wrongly."""

    def check(self, touched: list[str] | None, merge_group: bool = False) -> int:
        self.add_whole_tree()
        self.write("tools/scripts/test_a.py", "")
        self.script_test("a-selftest", "tools/scripts/test_a.py")
        self.generate()
        config = self.root / ".shipyard" / "config.toml"
        config.parent.mkdir(parents=True, exist_ok=True)
        # A committed block that no longer matches what the generator emits.
        config.write_text(f"{families.BEGIN}\nstale\n{families.END}\n", encoding="utf-8")
        saved_reply = inventory.codemodel_reply_available
        inventory.codemodel_reply_available = lambda _build: True
        saved = (inventory.load_ctest_json, inventory.load_codemodel_targets,
                 families.script_test_inputs.resolve_base, families.script_test_inputs.changed_files,
                 families.script_test_inputs.advisory_here)
        inventory.load_ctest_json = lambda _build: self.tests
        inventory.load_codemodel_targets = lambda _build: model()
        families.script_test_inputs.resolve_base = lambda _root, _base: None if touched is None else "base"
        families.script_test_inputs.changed_files = lambda _root, _base: set(touched or [])
        families.script_test_inputs.advisory_here = lambda: merge_group
        try:
            return families.main(["--repo-root", str(self.root), "--build-dir", "/b", "--check"])
        finally:
            inventory.codemodel_reply_available = saved_reply
            (inventory.load_ctest_json, inventory.load_codemodel_targets,
             families.script_test_inputs.resolve_base, families.script_test_inputs.changed_files,
             families.script_test_inputs.advisory_here) = saved

    def test_drift_blocks_a_change_to_a_mapped_surface(self) -> None:
        for path in ["tools/scripts/test_a.py", ".agents/skills/ci/SKILL.md",
                     "test/ctest_script_inputs.json", ".shipyard/config.toml",
                     "tools/scripts/changed_surface_script_families.py"]:
            with self.subTest(path=path):
                self.assertEqual(self.check([path]), 1)

    def test_missing_codemodel_is_a_reported_skip(self) -> None:
        saved = inventory.codemodel_reply_available
        inventory.codemodel_reply_available = lambda _build: False
        try:
            self.assertEqual(families.main(["--repo-root", str(self.root), "--build-dir", "/b",
                                            "--check"]), families.SKIP_EXIT)
        finally:
            inventory.codemodel_reply_available = saved

    def test_drift_without_a_base_is_a_full_blocking_check(self) -> None:
        self.assertEqual(self.check(None), 1)

    def test_drift_is_advisory_for_unrelated_changes_and_in_merge_groups(self) -> None:
        self.assertEqual(self.check(["core/view/src/widgets.cpp", "docs/guides/local-ci.md"]), 0)
        self.assertEqual(self.check(["tools/scripts/test_a.py"], merge_group=True), 0)


if __name__ == "__main__":
    unittest.main()
