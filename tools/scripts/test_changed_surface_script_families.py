#!/usr/bin/env python3
"""Contract for the generated changed-surface script families."""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path
from typing import Iterable

import changed_surface_inventory as inventory
import changed_surface_script_families as families
import wide_non_native


BUILD = "/b"


def ctest(name: str, *command: str, labels: list[str] | None = None,
          fixtures: list[str] | None = None, fixture_setup: list[str] | None = None,
          lock: list[str] | None = None) -> dict:
    properties = [{"name": "WORKING_DIRECTORY", "value": "/repo"}]
    if lock:
        properties.append({"name": "RESOURCE_LOCK", "value": lock})
    if labels:
        properties.append({"name": "LABELS", "value": labels})
    if fixtures:
        properties.append({"name": "FIXTURES_REQUIRED", "value": fixtures})
    if fixture_setup:
        properties.append({"name": "FIXTURES_SETUP", "value": fixture_setup})
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
                    fixtures: list[str] | None = None, fixture_setup: list[str] | None = None,
                    lock: list[str] | None = None) -> None:
        self.declared[name] = {"entry": entry, "inputs": sorted(set((inputs or []) + [entry])),
                               "kind": "python"}
        self.tests.append(ctest(name, *(command or ["python3", entry]), labels=labels,
                                fixtures=fixtures, fixture_setup=fixture_setup, lock=lock))

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



_LIVE: dict[str, object] = {}


def live_reachability() -> tuple[Path, list[str], dict, set[str]]:
    """native_reachable() over the live tree, computed once for the tests
    that compare against it: (root, top-level scripts, declared, reached)."""
    if not _LIVE:
        root = Path(__file__).resolve().parents[2]
        tracked = families.tracked_files(root)
        declared = json.loads((root / families.SCRIPT_INPUTS).read_text(encoding="utf-8"))["tests"]
        scripts = families.top_level_scripts(tracked)
        reached = families.native_reachable(scripts, tracked, {e.get("entry") for e in declared.values()},
                                            lambda rel: families.read_text(root, rel))
        _LIVE.update(root=root, scripts=scripts, declared=declared, reached=reached)
    return _LIVE["root"], _LIVE["scripts"], _LIVE["declared"], _LIVE["reached"]


class NamesStemTest(unittest.TestCase):
    def test_the_word_set_answers_exactly_what_the_whole_word_regex_does(self) -> None:
        texts = ["import foo_bar", "x = foo_barbaz", "foo_bar.run()", "foo_bar", "a-foo_bar-b",
                 "_foo_bar", "foo_bar_", "'tools/scripts/foo_bar.py'", "foo\nbar", "", "café foo_bar",
                 "foo-bar is a path", "use foo-bar.py", "xfoo-bar"]
        stems = ["foo_bar", "foo", "bar", "foo-bar", "caf", "x"]
        for text in texts:
            words = families.word_set(text)
            for stem in stems:
                with self.subTest(text=text, stem=stem):
                    self.assertEqual(families.names_stem(stem, text, words),
                                     re.search(r"\b" + re.escape(stem) + r"\b", text) is not None)


class GitBatchTransportTest(unittest.TestCase):
    """Large batch requests must not depend on anonymous pipe capacity."""

    def test_cat_file_batch_keeps_large_request_and_response_off_pipes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            original_run = families.subprocess.run

            def checked_run(*args, **kwargs):
                if kwargs.get("stdin") is not None:
                    self.assertIsNot(kwargs.get("stdout"), subprocess.PIPE)
                    self.assertNotIn("capture_output", kwargs)
                return original_run(*args, **kwargs)

            families.subprocess.run = checked_run
            try:
                output = families._git(
                    root, "cat-file", "--batch", stdin=(b"deadbeef\n" * 20_000)
                )
            finally:
                families.subprocess.run = original_run
            self.assertEqual(output.count(b"deadbeef missing\n"), 20_000)

    def test_cat_file_batch_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            payload = b"x" * 512
            blob = subprocess.run(
                ["git", "-C", str(root), "hash-object", "-w", "--stdin"],
                input=payload,
                check=True,
                capture_output=True,
            ).stdout.strip().decode("ascii")
            requests = (blob + "\n") * 512
            output = families._git(root, "cat-file", "--batch", stdin=requests.encode("ascii"))
            self.assertEqual(output.count(f"{blob} blob {len(payload)}\n".encode("ascii")), 512)
            self.assertEqual(output.count(payload), 512)

    def test_git_batch_request_uses_deadlock_safe_stdin(self) -> None:
        """A cat-file-like producer may fill stdout before reading its request."""
        fake_git = """#!/usr/bin/env python3
import sys
sys.stdout.write('x' * 4_000_000)
sys.stdout.flush()
request = sys.stdin.buffer.read()
sys.stdout.write(str(len(request)))
sys.stdout.flush()
"""
        probe = """import pathlib, sys
sys.path.insert(0, sys.argv[1])
import changed_surface_script_families as families
import subprocess
original_run = families.subprocess.run
def checked_run(*args, **kwargs):
    if kwargs.get("stdin") is not None:
        assert kwargs.get("stdout") is not subprocess.PIPE
        assert "capture_output" not in kwargs
    return original_run(*args, **kwargs)
families.subprocess.run = checked_run
request = (b"deadbeef\\n" * 20000)
result = families._git(pathlib.Path('.'), 'cat-file', '--batch', stdin=request)
print(result[-len(str(len(request))):].decode())
"""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake = root / "git"
            fake.write_text(fake_git, encoding="utf-8")
            fake.chmod(0o755)
            env = dict(os.environ)
            env["PATH"] = f"{root}{os.pathsep}{env.get('PATH', '')}"
            completed = subprocess.run(
                [sys.executable, "-c", probe, str(Path(__file__).resolve().parent)],
                cwd=root, env=env, text=True, capture_output=True, timeout=10, encoding="utf-8"
            )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stdout.strip(), str(len(b"deadbeef\n" * 20000)))


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

    def test_the_static_predictor_agrees_with_native_reachable_on_the_live_tree(self) -> None:
        # The fixture case above proves the shared seed rule; this one checks
        # the predictor's backwards walk against the configured forward walk
        # on real script bodies, over a fixed sample of each side (the full
        # set costs about 40 s more than the sample).
        root, scripts, declared, configured = live_reachability()
        prediction = families.Prediction(families.Snapshot(root, "HEAD", {}), declared)

        def sample(pool: Iterable[str]) -> list[str]:
            return sorted(pool, key=lambda s: hashlib.sha256(s.encode()).hexdigest())[:20]
        reachable, unreachable = sample(configured), sample(set(scripts) - configured)
        # Control: both sides of the sample are populated, so the tree was read.
        self.assertEqual((len(reachable), len(unreachable)), (20, 20))
        self.assertEqual([s for s in reachable + unreachable if prediction.reached(s) != (s in configured)], [])

    def test_the_live_tree_keeps_a_reviewed_scanner_list_out_of_script_bodies(self) -> None:
        # A script body that names a path makes it reachable, so a list of
        # reviewed paths kept in a reachable script would unmap them all. The
        # list lives in tools/ci data; the runner's own test stays mappable.
        _, _, _, reached = live_reachability()
        # Control: the scanner test itself is reachable, so the walk did run.
        self.assertIn("tools/scripts/test_wide_non_native.py", reached)
        self.assertNotIn("tools/scripts/test_run_changed_surface_tests.py", reached)

    def test_a_reachable_script_naming_a_test_unmaps_it(self) -> None:
        # The negative control for the live check above: the same naming,
        # inside a reachable script, does unmap.
        self.add_whole_tree()
        self.write("tools/scripts/scanner.py", 'REVIEWED = {"tools/scripts/test_quiet.py": "ok"}\n')
        self.write("tools/cli/cmd_scan.cpp", 'auto s = "tools/scripts/scanner.py";\n')
        self.write("tools/scripts/test_quiet.py", "")
        self.script_test("quiet-selftest", "tools/scripts/test_quiet.py", [])
        self.assertEqual(self.family_for(self.generate(), "tools/scripts/test_quiet.py"), [])

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

    def predicted_and_configured(self) -> tuple[set[str], set[str]]:
        """Reachability as the static predictor and as native_reachable()
        decide it, over the fixture committed as one revision."""
        self.write("test/ctest_script_inputs.json",
                   json.dumps({"schema": "pulp-ctest-script-inputs/v1", "tests": self.declared,
                               "executables": {}}))
        git = ["git", "-C", str(self.root), "-c", "user.name=t", "-c", "user.email=t@t"]
        subprocess.run(git + ["add", "-A"], check=True)
        subprocess.run(git + ["commit", "-q", "-m", "fixture"], check=True)
        tracked = families.tracked_files(self.root)
        scripts = families.top_level_scripts(tracked)
        entries = {e.get("entry") for e in self.declared.values()}
        configured = families.native_reachable(scripts, tracked, entries,
                                               lambda rel: families.read_text(self.root, rel))
        prediction = families.Prediction(families.Snapshot(self.root, "HEAD", {}), self.declared)
        return {s for s in scripts if prediction.reached(s)}, configured

    def test_the_static_predictor_agrees_with_native_reachable_on_an_entry_cmake_runs(self) -> None:
        self.write("tools/scripts/tool.py", "")
        self.write("tools/scripts/quiet.py", "")
        self.write("tools/scripts/uses_tool.py", "import tool\n")
        self.write("test/cmake/tests.cmake",
                   "add_test(NAME tool-selftest COMMAND python3 tools/scripts/tool.py)\n"
                   "add_custom_command(TARGET probe PRE_LINK COMMAND python3 tools/scripts/tool.py)\n"
                   "add_test(NAME quiet COMMAND python3 tools/scripts/quiet.py)\n")
        self.script_test("tool-selftest", "tools/scripts/tool.py", [])
        self.script_test("quiet", "tools/scripts/quiet.py", [])
        predicted, configured = self.predicted_and_configured()
        # tool.py is reached through the custom command, and the script that
        # names it is not (reachability flows from a seed to what it names).
        self.assertEqual(configured, {"tools/scripts/tool.py"})
        self.assertEqual(predicted, configured)

    def test_a_test_entry_cmake_also_runs_stays_unmapped(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/test_tool.py", "")
        self.write("tools/scripts/tool.py", "")
        self.write("tools/scripts/test_quiet.py", "")
        self.write("tools/scripts/quiet.py", "")
        self.write("tools/scripts/hashed.py", "")
        self.write("test/cmake/tests.cmake",
                   "add_test(NAME tool-selftest COMMAND python3 tools/scripts/tool.py --self-test)\n"
                   "add_custom_command(TARGET probe PRE_LINK COMMAND python3 tools/scripts/tool.py)\n"
                   "# tools/scripts/quiet.py is only described here\n"
                   'add_test(NAME quiet COMMAND python3 tools/scripts/quiet.py "#not-a-comment")\n'
                   'add_test(NAME hashed COMMAND python3 tools/scripts/hashed.py)\n'
                   'add_custom_command(OUTPUT x COMMAND echo "#1" && python3 tools/scripts/hashed.py)\n')
        self.script_test("tool-selftest", "tools/scripts/tool.py", [])
        self.script_test("quiet", "tools/scripts/quiet.py", [])
        self.script_test("hashed", "tools/scripts/hashed.py", [])
        generated = self.generate()
        # A link step runs tool.py, so a change to it must plan the full suite.
        self.assertEqual(self.family_for(generated, "tools/scripts/tool.py"), [])
        # A quoted "#" is not a comment: the script after it is still seen.
        self.assertEqual(self.family_for(generated, "tools/scripts/hashed.py"), [])
        # Control: an entry CMake names only in add_test and a comment maps.
        self.assertNotEqual(self.family_for(generated, "tools/scripts/quiet.py"), [])

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

    def test_reader_with_a_registered_fixture_setup_is_mappable(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/test_setup.py", "")
        self.write("tools/scripts/test_fixture.py", "")
        self.script_test("fixture-setup", "tools/scripts/test_setup.py", fixture_setup=["setup"])
        self.script_test("fixture-selftest", "tools/scripts/test_fixture.py", fixtures=["setup"])
        generated = self.generate()
        self.assertNotEqual(self.family_for(generated, "tools/scripts/test_fixture.py"), [])

    def test_fixture_reader_builds_the_registered_setup_target(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/test_setup.py", "")
        self.write("tools/scripts/test_fixture.py", "")
        self.script_test("fixture-setup", "tools/scripts/test_setup.py",
                         command=["/b/bin/setup"], fixture_setup=["setup"])
        self.script_test("fixture-selftest", "tools/scripts/test_fixture.py",
                         command=["/b/bin/reader"], fixtures=["setup"])
        generated = self.generate(("fixture-setup", "/b/bin/setup"),
                                  ("fixture-reader", "/b/bin/reader"))
        owners = [name for name in self.family_for(generated, "tools/scripts/test_fixture.py")
                  if name != "script-surface-whole-tree"]
        self.assertEqual(len(owners), 1)
        self.assertEqual(generated[owners[0]]["build_targets"],
                         ["fixture-reader", "fixture-setup", "pulp-cli"])

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

    def test_the_families_file_is_what_the_gate_widening_treats_as_selection_only(self) -> None:
        self.assertEqual(str(families.FAMILIES_FILE), wide_non_native.SELECTOR_FAMILIES_FILE)

    def test_optional_environment_tests_are_excluded_from_the_generated_family(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/optional_test.py", "")
        self.script_test("optional-selftest", "tools/scripts/optional_test.py",
                         labels=["browser-capture"])
        # Real CTest JSON commonly exports this property as the string
        # ``TRUE``.  Keep the native-bool form covered by the fixture helper's
        # older contract in a separate test below.
        self.tests[-1]["properties"].append({"name": "PULP_OPTIONAL", "value": "TRUE"})
        self.write("tools/scripts/required_test.py", "")
        self.script_test("required-selftest", "tools/scripts/required_test.py",
                         labels=["browser-capture"])
        generated = self.generate()
        self.assertEqual(generated["script-surface-environment-bound"]["tests"],
                         ["required-selftest"])

    def test_optional_environment_normalization_keeps_false_required(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/optional_true.py", "")
        self.script_test("optional-true", "tools/scripts/optional_true.py",
                         labels=["browser-capture"])
        self.tests[-1]["properties"].append({"name": "PULP_OPTIONAL", "value": " true "})
        self.write("tools/scripts/optional_false.py", "")
        self.script_test("optional-false", "tools/scripts/optional_false.py",
                         labels=["browser-capture"])
        self.tests[-1]["properties"].append({"name": "PULP_OPTIONAL", "value": "FALSE"})
        self.write("tools/scripts/native_bool.py", "")
        self.script_test("native-bool", "tools/scripts/native_bool.py",
                         labels=["browser-capture"])
        self.tests[-1]["properties"].append({"name": "PULP_OPTIONAL", "value": True})
        generated = self.generate()
        self.assertEqual(generated["script-surface-environment-bound"]["tests"],
                         ["optional-false"])

    def test_no_whole_tree_test_refuses_to_bound_anything(self) -> None:
        self.write("tools/scripts/test_alone.py", "")
        self.script_test("alone-selftest", "tools/scripts/test_alone.py")
        with self.assertRaises(families.GenerationError):
            self.generate()

    def test_rendered_file_holds_only_families_and_is_valid_toml(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/test_a.py", "")
        self.script_test("a-selftest", "tools/scripts/test_a.py")
        generated = self.generate()
        parsed = tomllib.loads(families.render(list(generated.values())))
        self.assertEqual(set(parsed), {"families"})
        self.assertEqual({f["name"] for f in parsed["families"]}, set(generated))
        self.assertEqual(families.render(list(generated.values())),
                         families.render(list(self.generate().values())))


class DriftCheckTest(FamilyFixture):
    """`--check` blocks only a change a stale family could bound wrongly."""

    def check(self, touched: list[str] | None, merge_group: bool = False,
              build: str = "/b", mode: str = "--check") -> int:
        self.add_whole_tree()
        self.write("tools/scripts/test_a.py", "")
        self.script_test("a-selftest", "tools/scripts/test_a.py")
        self.generate()
        committed = self.root / families.FAMILIES_FILE
        committed.parent.mkdir(parents=True, exist_ok=True)
        # A committed file that no longer matches what the generator emits.
        committed.write_text("# stale\n", encoding="utf-8")
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
            return families.main(["--repo-root", str(self.root), "--build-dir", build, mode])
        finally:
            inventory.codemodel_reply_available = saved_reply
            (inventory.load_ctest_json, inventory.load_codemodel_targets,
             families.script_test_inputs.resolve_base, families.script_test_inputs.changed_files,
             families.script_test_inputs.advisory_here) = saved

    def test_drift_blocks_a_change_to_a_mapped_surface(self) -> None:
        for path in ["tools/scripts/test_a.py", ".agents/skills/ci/SKILL.md",
                     "test/ctest_script_inputs.json", ".shipyard/config.toml",
                     ".shipyard/changed-surface-families.toml",
                     "tools/scripts/changed_surface_script_families.py"]:
            with self.subTest(path=path):
                self.assertEqual(self.check([path]), 1)

    def build_with_cache(self, **cache: str) -> str:
        build = self.root / "build-profile"
        build.mkdir(exist_ok=True)
        (build / "CMakeCache.txt").write_text(
            "".join(f"{key}:STRING={value}\n" for key, value in cache.items()), encoding="utf-8")
        return str(build)

    def test_a_gate_profile_build_still_blocks_drift(self) -> None:
        build = self.build_with_cache(CMAKE_BUILD_TYPE="Release",
                                      PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF="OFF")
        self.assertEqual(self.check(["tools/scripts/test_a.py"], build=build), 1)

    def test_a_build_outside_the_gate_profile_skips_the_check(self) -> None:
        """The proof option registers GPU probes the gate never does, so a
        file written or judged on such a build disagrees with the gate's."""
        for cache in ({"CMAKE_BUILD_TYPE": "Release", "PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF": "ON"},
                      {"CMAKE_BUILD_TYPE": "Debug"}):
            with self.subTest(cache=cache):
                build = self.build_with_cache(**cache)
                self.assertEqual(self.check(["tools/scripts/test_a.py"], build=build),
                                 families.SKIP_EXIT)

    def test_a_build_outside_the_gate_profile_refuses_to_write(self) -> None:
        build = self.build_with_cache(CMAKE_BUILD_TYPE="Release",
                                      PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF="ON")
        self.assertEqual(self.check(["tools/scripts/test_a.py"], build=build, mode="--write"), 2)
        self.assertEqual((self.root / families.FAMILIES_FILE).read_text(encoding="utf-8"),
                         "# stale\n")

    def test_missing_codemodel_is_a_reported_skip(self) -> None:
        saved = inventory.codemodel_reply_available
        inventory.codemodel_reply_available = lambda _build: False
        try:
            self.assertEqual(families.main(["--repo-root", str(self.root), "--build-dir", "/b",
                                            "--check"]), families.SKIP_EXIT)
        finally:
            inventory.codemodel_reply_available = saved

    def test_drift_report_names_the_paths_that_moved(self) -> None:
        self.add_whole_tree()
        self.write("tools/scripts/test_a.py", "")
        self.script_test("a-selftest", "tools/scripts/test_a.py")
        current = families.render(list(self.generate().values()))
        stale = current.replace('"a-selftest"', '"old-selftest"')
        self.assertIn("tools/scripts/test_a.py: readers +['a-selftest'] -['old-selftest']",
                      families.describe_drift(stale, current))
        self.assertIn("tools/scripts/test_a.py: newly mapped",
                      families.describe_drift("", current))

    def test_drift_without_a_base_is_a_full_blocking_check(self) -> None:
        self.assertEqual(self.check(None), 1)

    def test_drift_is_advisory_for_unrelated_changes_and_in_merge_groups(self) -> None:
        self.assertEqual(self.check(["core/view/src/widgets.cpp", "docs/guides/local-ci.md"]), 0)
        self.assertEqual(self.check(["tools/scripts/test_a.py"], merge_group=True), 0)


class StaticCheckTest(FamilyFixture):
    """`--static` blocks a script the change maps or unmaps without a regenerated
    families file, with no configured build, and stays quiet otherwise."""

    def setUp(self) -> None:
        super().setUp()
        for key, value in (("user.email", "t@example.com"), ("user.name", "t")):
            subprocess.run(["git", "-C", str(self.root), "config", key, value], check=True)
        self.add_whole_tree()
        self.write("tools/scripts/test_a.py", "")
        self.script_test("a-selftest", "tools/scripts/test_a.py")
        self.regenerate()
        self.commit("base")
        subprocess.run(["git", "-C", str(self.root), "branch", "base"], check=True)

    def regenerate(self) -> None:
        self.write(str(families.FAMILIES_FILE), families.render(list(self.generate().values())))

    def commit(self, message: str) -> None:
        self.write("test/ctest_script_inputs.json",
                   json.dumps({"schema": "pulp-ctest-script-inputs/v1", "tests": self.declared,
                               "executables": {}}))
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)
        subprocess.run(["git", "-C", str(self.root), "commit", "-q", "-m", message], check=True)

    def static(self) -> tuple[list[str], list[str], int]:
        return families.static_drift(self.root, "base")

    def static_exit(self) -> int:
        # A merge-group job's environment would make the verdict advisory.
        saved = families.script_test_inputs.advisory_here
        families.script_test_inputs.advisory_here = lambda: False
        try:
            return families.main(["--repo-root", str(self.root), "--static", "--base", "base"])
        finally:
            families.script_test_inputs.advisory_here = saved

    def add_script_test(self) -> None:
        self.write("tools/scripts/new_check.py", "")
        self.write("tools/scripts/test_new_check.py", "import new_check\n")
        self.script_test("new-check-selftest", "tools/scripts/test_new_check.py",
                         ["tools/scripts/new_check.py"])

    def test_a_new_script_test_without_a_regeneration_blocks(self) -> None:
        self.add_script_test()
        self.commit("add a script test, families file untouched")
        blocking, _advisory, _n = self.static()
        self.assertEqual(blocking, ["tools/scripts/new_check.py: newly mapped",
                                    "tools/scripts/test_new_check.py: newly mapped"])
        self.assertEqual(self.static_exit(), 1)

    def test_the_same_change_with_the_file_regenerated_passes(self) -> None:
        self.add_script_test()
        self.regenerate()
        self.commit("add a script test and regenerate")
        self.assertEqual(self.static()[0], [])
        self.assertEqual(self.static_exit(), 0)

    def test_a_removed_script_without_a_regeneration_blocks(self) -> None:
        (self.root / "tools/scripts/test_a.py").unlink()
        del self.declared["a-selftest"]
        self.tests = [t for t in self.tests if t["name"] != "a-selftest"]
        self.commit("remove a script test, families file untouched")
        self.assertEqual(self.static()[0], ["tools/scripts/test_a.py: no longer mapped"])

    def test_a_script_native_code_names_stays_unmapped_and_does_not_block(self) -> None:
        self.write("tools/scripts/runner.py", "import new_check\n")
        self.write("tools/cli/run.sh", "python3 tools/scripts/runner.py\n")
        self.write("tools/scripts/new_check.py", "")
        self.script_test("runner-selftest", "tools/scripts/runner.py", ["tools/scripts/new_check.py"])
        self.regenerate()
        generated_paths = (self.root / families.FAMILIES_FILE).read_text(encoding="utf-8")
        self.assertNotIn("tools/scripts/new_check.py", generated_paths)  # reached through runner.py
        self.commit("a script a shell script runs")
        self.assertEqual(self.static()[0], [])

    def test_a_mapping_the_prediction_cannot_see_is_cancelled_by_the_base(self) -> None:
        # The fixture blocks the mapping only in the configured generator; the
        # prediction cannot see fixtures, and disagrees at the base as at HEAD.
        self.write("tools/scripts/test_fixture.py", "")
        self.script_test("fixture-selftest", "tools/scripts/test_fixture.py", fixtures=["setup"])
        self.regenerate()
        self.commit("a fixture reader")
        subprocess.run(["git", "-C", str(self.root), "branch", "-f", "base"], check=True)
        self.write("tools/scripts/test_fixture.py", "# edited\n")
        self.commit("edit it")
        self.assertEqual(self.static()[0], [])

    def test_an_unrelated_change_examines_nothing(self) -> None:
        self.write("core/x.cpp", "int x;\n")
        self.commit("native only")
        self.assertEqual(self.static(), ([], [], 0))

    def test_the_rendered_file_reads_back_without_tomllib(self) -> None:
        self.add_script_test()
        text = families.render(list(self.generate().values()))
        self.assertEqual(families._parse_rendered(text), tomllib.loads(text)["families"])


if __name__ == "__main__":
    unittest.main()
