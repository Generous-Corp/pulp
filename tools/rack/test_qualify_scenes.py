#!/usr/bin/env python3
"""Provider-free contract tests for the shipped-scene qualification driver."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import qualify_scenes as Q


def macho(code: bytes, signature: bytes) -> bytes:
    header = struct.pack("<IiiIIIII", 0xFEEDFACF, 0, 0, 2, 1, 16, 0, 0)
    signature_offset = len(header) + 16 + len(code)
    command = struct.pack("<IIII", 0x1D, 16, signature_offset, len(signature))
    return header + command + code + signature


class Fixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.toolchain = root / "tools"
        self.toolchain.mkdir()
        binary_identity = root / "examples/forge-modular/binary_identity.py"
        binary_identity.parent.mkdir(parents=True)
        shutil.copy2(
            Path(Q.__file__).resolve().parents[2] /
            "examples/forge-modular/binary_identity.py",
            binary_identity)
        for name in ("patch.py", "with_app_model_selection.py", "idiom_check.py",
                     "fidelity.py", "rack_open.py"):
            (self.toolchain / name).write_text("# fixture\n", encoding="utf-8")
        (self.toolchain / "patch.py").write_text(
            "def inventory():\n"
            "    return {'Fixture': {'version': '1', 'modules': ['One']}}\n",
            encoding="utf-8")
        (self.toolchain / "FORGE_TOOLCHAIN_STAMP").write_text(
            "fixture-forge@abc pulp@def\n", encoding="utf-8")
        self.codex_home = root / "codex-home"
        self.codex_home.mkdir()
        self.codex = root / "codex"
        self.codex.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        self.codex.chmod(0o755)
        self.settings = root / "settings.json"
        self.settings.write_text(json.dumps({
            "ui_provider": "codex", "ui_model": "fixture-model",
            "reasoning_effort": "medium",
            "codex_executable": str(self.codex),
            "codex_home": str(self.codex_home),
        }) + "\n", encoding="utf-8")
        self.catalogue = root / "catalogue.json"
        self.catalogue.write_text(json.dumps({
            "schema": Q.CATALOGUE_SCHEMA,
            "scenes": [{"id": scene_id, "prompt": f"fixture prompt {scene_id}",
                        "idiom": f"fixture-idiom-{index:02d}"}
                       for index, scene_id in enumerate(Q.SCENE_IDS, 1)],
        }), encoding="utf-8")
        self.identity = root / "identity-input.json"
        files = {name: Q._file_digest(self.toolchain / name) for name in (
            "patch.py", "with_app_model_selection.py", "idiom_check.py",
            "fidelity.py", "rack_open.py", "FORGE_TOOLCHAIN_STAMP")}
        self.identity.write_text(json.dumps({
            "schema": Q.IDENTITY_SCHEMA,
            "identity": {"forge_revision": "abc", "pulp_revision": "def",
                         "provider": "codex", "model": "fixture-model",
                         "reasoning_effort": "medium",
                         "codex_executable": str(self.codex),
                         "codex_home": str(self.codex_home),
                         "toolchain": {
                             "stamp": "fixture-forge@abc pulp@def",
                             "files": files,
                         }},
        }), encoding="utf-8")
        self.inventory = root / "inventory-input.json"
        self.inventory.write_text(json.dumps({
            "schema": Q.INVENTORY_SCHEMA,
            "inventory": {"Fixture": {"version": "1", "modules": ["One"]}},
        }), encoding="utf-8")
        self.run_root = root / "run"

    def kwargs(self) -> dict:
        return dict(catalogue=self.catalogue, identity=self.identity,
                    inventory=self.inventory, toolchain=self.toolchain,
                    settings=self.settings, run_root=self.run_root,
                    inventory_loader=lambda _root: {
                        "Fixture": {"version": "1", "modules": ["One"]}},
                    selection_loader=lambda _root, _settings: {
                        "provider": "codex", "model": "fixture-model",
                        "reasoning_effort": "medium",
                        "codex_executable": str(self.codex),
                        "codex_home": str(self.codex_home),
                    })


class FakeRunner:
    def __init__(self, returncode: int = 0, reserve_empty: bool = False) -> None:
        self.returncode = returncode
        self.reserve_empty = reserve_empty
        self.commands: list[list[str]] = []

    def __call__(self, command, *, cwd, env, stdout, stderr, check):
        self.commands.append(command)
        assert Path(cwd).is_absolute()
        assert Path(command[1]).name == "with_app_model_selection.py"
        assert Path(command[1]).is_absolute()
        assert Path(command[command.index("--settings") + 1]).is_absolute()
        assert command[command.index("--role") + 1] == "ui"
        assert Path(command[command.index("--") + 2]).name == "patch.py"
        assert command[command.index("build") + 1].startswith("fixture prompt P05-S")
        assert Path(command[command.index("--out") + 1]).is_absolute()
        assert Path(env["FORGE_ATTEMPT_DIR"]).is_absolute()
        assert command[command.index("--retries") + 1] == "0"
        assert command.count("--retries") == 1
        if self.returncode == 0:
            artifact = Path(command[command.index("--out") + 1])
            artifact.write_text('{"modules":[{"id":1}],"cables":[]}\n',
                                encoding="utf-8")
            artifact.with_suffix(".why.json").write_text("{}\n", encoding="utf-8")
            Path(env["FORGE_MODEL_PROMPT_RECORD"]).write_text(
                "fixture full model prompt\n", encoding="utf-8")
            if not self.reserve_empty:
                attempts = Path(env["FORGE_ATTEMPT_DIR"])
                (attempts / "attempt01-model-response.txt").write_text(
                    "fixture raw response\n", encoding="utf-8")
        if self.reserve_empty:
            (Path(env["FORGE_ATTEMPT_DIR"]) /
             "attempt01-model-response.txt").write_text("", encoding="utf-8")
        stdout.write(f"fixture exit {self.returncode}\n")
        return subprocess.CompletedProcess(command, self.returncode)


class QualificationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.fixture = Fixture(Path(self.temp.name))

    def prepare_fixture(self) -> tuple[Path, dict]:
        forge_revision = "a" * 40
        pulp_revision = "b" * 40
        decoder = self.fixture.root / "build" / "rack_patch_decode"
        decoder.parent.mkdir(exist_ok=True)
        decoder.write_bytes(b"fixture decoder")
        decoder.chmod(0o755)
        executing_decoder = self.fixture.toolchain / "rack_patch_decode"
        executing_decoder.write_bytes(decoder.read_bytes())
        executing_decoder.chmod(0o755)
        self.fixture.toolchain.joinpath("with_app_model_selection.py").write_bytes(
            Path(Q.__file__).with_name("with_app_model_selection.py").read_bytes())
        self.fixture.toolchain.joinpath("FORGE_TOOLCHAIN_STAMP").write_text(
            f"0.21.0\n2026-08-30\npulp {pulp_revision}\n",
            encoding="utf-8")
        build_info = self.fixture.root / "FORGE_BUILD_INFO"
        fields = {
            "schema": "1",
            "version": "0.21.0",
            "product": "Forge Modular",
            "product_id": "com.generous.forge.modular",
            "role": "Rack module and patch generator",
            "format": "Standalone application",
            "build": "Release · darwin-arm64",
            "pulp_sdk": f"0.826.0 · {pulp_revision}",
            "source_git_head": forge_revision,
            "source_git_dirty": "false",
            "source_snapshot_sha256": "c" * 64,
        }
        build_info.write_text(
            "".join(f"{key}={value}\n" for key, value in fields.items()),
            encoding="utf-8")
        return build_info, fields

    def prepare_kwargs(self) -> dict:
        build_info, _fields = self.prepare_fixture()
        return {
            "toolchain": self.fixture.toolchain,
            "settings": self.fixture.settings,
            "build_info": build_info,
            "expected_pulp_ref": "b" * 40,
            "output_dir": self.fixture.root / "qualification-inputs",
        }

    def packaged_prepare_kwargs(self, bundle_name: str = "Forge Modular.app",
                                build_info_name: str = "FORGE_BUILD_INFO") -> dict:
        kwargs = self.prepare_kwargs()
        resources = self.fixture.root / bundle_name / "Contents/Resources"
        packaged_toolchain = resources / "tools/rack"
        shutil.copytree(self.fixture.toolchain, packaged_toolchain)
        packaged_toolchain.joinpath("rack_patch_decode").unlink()
        packaged_decoder = resources / "build/rack_patch_decode"
        packaged_decoder.parent.mkdir(parents=True)
        shutil.copy2(self.fixture.root / "build/rack_patch_decode",
                     packaged_decoder)
        packaged_identity = resources / "examples/forge-modular/binary_identity.py"
        packaged_identity.parent.mkdir(parents=True)
        shutil.copy2(
            self.fixture.root / "examples/forge-modular/binary_identity.py",
            packaged_identity)
        packaged_build_info = resources / build_info_name
        shutil.copy2(kwargs["build_info"], packaged_build_info)
        kwargs.update({
            "toolchain": packaged_toolchain,
            "build_info": packaged_build_info,
            "output_dir": self.fixture.root / "packaged-qualification-inputs",
        })
        return kwargs

    def test_catalogue_enumerates_exact_fixed_ids_in_order(self) -> None:
        scenes = Q.load_catalogue(self.fixture.catalogue)
        self.assertEqual([scene["id"] for scene in scenes], list(Q.SCENE_IDS))
        self.assertEqual(len(scenes), 12)
        document = json.loads(self.fixture.catalogue.read_text())
        document["scenes"][0], document["scenes"][1] = (
            document["scenes"][1], document["scenes"][0])
        self.fixture.catalogue.write_text(json.dumps(document))
        with self.assertRaisesRegex(Q.QualificationError, "must be P05-S01"):
            Q.load_catalogue(self.fixture.catalogue)

    def test_list_cli_is_provider_free_and_preserves_catalogue_order(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(Path(Q.__file__)),
             "--catalogue", str(self.fixture.catalogue), "--list"],
            check=True, text=True, capture_output=True)
        scenes = [json.loads(line) for line in completed.stdout.splitlines()]
        self.assertEqual([scene["id"] for scene in scenes], list(Q.SCENE_IDS))

    def test_real_selection_and_inventory_loaders_are_provider_free(self) -> None:
        selected = Q.load_model_selection(Path(Q.__file__).parent,
                                          self.fixture.settings)
        self.assertEqual(selected["model"], "fixture-model")
        self.assertEqual(Q.snapshot_inventory(self.fixture.toolchain), {
            "Fixture": {"version": "1", "modules": ["One"]}})

    def test_prepare_atomically_authors_provider_free_receipts(self) -> None:
        kwargs = self.prepare_kwargs()
        identity_path, inventory_path = Q.prepare_inputs(**kwargs)
        identity = json.loads(identity_path.read_text())
        inventory = json.loads(inventory_path.read_text())
        self.assertEqual(identity["schema"], Q.IDENTITY_SCHEMA)
        self.assertEqual(identity["identity"]["forge_revision"], "a" * 40)
        self.assertEqual(identity["identity"]["pulp_revision"], "b" * 40)
        self.assertEqual(identity["identity"]["provider"], "codex")
        self.assertEqual(identity["identity"]["model"], "fixture-model")
        self.assertEqual(inventory, {
            "schema": Q.INVENTORY_SCHEMA,
            "inventory": {"Fixture": {"modules": ["One"], "version": "1"}},
        })
        before = (identity_path.read_bytes(), inventory_path.read_bytes())
        self.assertEqual(Q.prepare_inputs(**kwargs), (identity_path, inventory_path))
        self.assertEqual(
            (identity_path.read_bytes(), inventory_path.read_bytes()), before)

    def test_prepare_cli_needs_no_catalogue_or_provider_call(self) -> None:
        kwargs = self.prepare_kwargs()
        completed = subprocess.run([
            sys.executable, str(Path(Q.__file__)), "prepare",
            "--toolchain-root", str(kwargs["toolchain"]),
            "--settings", str(kwargs["settings"]),
            "--build-info", str(kwargs["build_info"]),
            "--expected-pulp-ref", kwargs["expected_pulp_ref"],
            "--output-dir", str(kwargs["output_dir"]),
        ], text=True, capture_output=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stdout.splitlines(), [
            str(kwargs["output_dir"].absolute() / "identity.json"),
            str(kwargs["output_dir"].absolute() / "inventory.json"),
        ])

    def test_prepare_accepts_the_real_packaged_resources_layout(self) -> None:
        kwargs = self.packaged_prepare_kwargs()
        decoder = kwargs["build_info"].parent / "build/rack_patch_decode"
        self.assertFalse(kwargs["toolchain"].joinpath("rack_patch_decode").exists())

        identity_path, inventory_path = Q.prepare_inputs(**kwargs)

        identity = json.loads(identity_path.read_text())["identity"]
        self.assertEqual(identity["rack_patch_decode_sha256"],
                         Q._binary_identity(kwargs["build_info"].parent, decoder))
        self.assertTrue(inventory_path.is_file())

    def test_packaged_prepare_rejects_decoder_identity_mutation(self) -> None:
        kwargs = self.packaged_prepare_kwargs()
        executing_decoder = kwargs["toolchain"] / "rack_patch_decode"
        executing_decoder.write_bytes(b"stale duplicate decoder")
        executing_decoder.chmod(0o755)

        with self.assertRaisesRegex(Q.QualificationError,
                                    "executing.*does not match"):
            Q.prepare_inputs(**kwargs)
        self.assertFalse(kwargs["output_dir"].exists())

    def test_prepare_does_not_exempt_the_same_layout_outside_an_app(self) -> None:
        kwargs = self.packaged_prepare_kwargs(bundle_name="Forge Modular")

        with self.assertRaisesRegex(Q.QualificationError,
                                    "decoder is missing or not executable"):
            Q.prepare_inputs(**kwargs)
        self.assertFalse(kwargs["output_dir"].exists())

    def test_prepare_does_not_exempt_a_renamed_build_info(self) -> None:
        kwargs = self.packaged_prepare_kwargs(build_info_name="BUILD_INFO")

        with self.assertRaisesRegex(Q.QualificationError,
                                    "decoder is missing or not executable"):
            Q.prepare_inputs(**kwargs)
        self.assertFalse(kwargs["output_dir"].exists())

    def test_prepare_rejects_build_identity_mutations_without_partial_output(self) -> None:
        mutations = {
            "dirty": ("source_git_dirty=false", "source_git_dirty=true"),
            "wrong product": ("product=Forge Modular", "product=Forge Sequencer"),
            "wrong build": ("build=Release · darwin-arm64",
                            "build=Debug · darwin-arm64"),
            "version disagreement": ("version=0.21.0", "version=0.21.1"),
            "wrong source": ("source_git_head=" + "a" * 40,
                             "source_git_head=not-a-sha"),
            "wrong snapshot": ("source_snapshot_sha256=" + "c" * 64,
                               "source_snapshot_sha256=not-a-sha"),
        }
        for label, (old, new) in mutations.items():
            with self.subTest(label=label):
                kwargs = self.prepare_kwargs()
                text = kwargs["build_info"].read_text().replace(old, new)
                kwargs["build_info"].write_text(text)
                with self.assertRaises(Q.QualificationError):
                    Q.prepare_inputs(**kwargs)
                self.assertFalse(kwargs["output_dir"].exists())

    def test_prepare_rejects_duplicate_build_field_and_stamp_disagreement(self) -> None:
        kwargs = self.prepare_kwargs()
        with kwargs["build_info"].open("a", encoding="utf-8") as output:
            output.write("product=Forge Modular\n")
        with self.assertRaisesRegex(Q.QualificationError, "repeats field"):
            Q.prepare_inputs(**kwargs)
        self.assertFalse(kwargs["output_dir"].exists())

        kwargs = self.prepare_kwargs()
        self.fixture.toolchain.joinpath("FORGE_TOOLCHAIN_STAMP").write_text(
            "0.21.0\n2026-08-30\npulp " + "d" * 40 + "\n"
            "rack_patch_decode " + "e" * 64 + "\n")
        with self.assertRaisesRegex(Q.QualificationError, "different Pulp"):
            Q.prepare_inputs(**kwargs)
        self.assertFalse(kwargs["output_dir"].exists())

        kwargs = self.prepare_kwargs()
        self.fixture.toolchain.joinpath("FORGE_TOOLCHAIN_STAMP").write_text(
            "0.21.0\n2026-08-30\npulp " + "b" * 40 + "\n")
        self.fixture.root.joinpath("build/rack_patch_decode").unlink()
        with self.assertRaisesRegex(Q.QualificationError, "not executable"):
            Q.prepare_inputs(**kwargs)
        self.assertFalse(kwargs["output_dir"].exists())

    def test_prepare_binds_optional_stamp_decoder_to_candidate_executable(self) -> None:
        kwargs = self.prepare_kwargs()
        decoder = self.fixture.root / "build" / "rack_patch_decode"
        digest = Q._file_digest(decoder)
        stamp = self.fixture.toolchain / "FORGE_TOOLCHAIN_STAMP"
        stamp.write_text(stamp.read_text() + f"rack_patch_decode {digest}\n")
        identity_path, _inventory_path = Q.prepare_inputs(**kwargs)
        identity = json.loads(identity_path.read_text())["identity"]
        self.assertEqual(identity["rack_patch_decode_sha256"], digest)

        kwargs["output_dir"].rename(self.fixture.root / "accepted-inputs")
        stamp.write_text(
            "0.21.0\n2026-08-30\npulp " + "b" * 40 + "\n"
            "rack_patch_decode " + "e" * 64 + "\n")
        with self.assertRaisesRegex(Q.QualificationError, "candidate executable"):
            Q.prepare_inputs(**kwargs)
        self.assertFalse(kwargs["output_dir"].exists())

    def test_prepare_rejects_executing_decoder_different_from_candidate(self) -> None:
        kwargs = self.prepare_kwargs()
        executing_decoder = self.fixture.toolchain / "rack_patch_decode"
        executing_decoder.write_bytes(b"different decoder")
        with self.assertRaisesRegex(Q.QualificationError, "executing.*does not match"):
            Q.prepare_inputs(**kwargs)
        self.assertFalse(kwargs["output_dir"].exists())

    def test_prepare_accepts_a_decoder_with_only_its_signature_replaced(self) -> None:
        kwargs = self.prepare_kwargs()
        candidate = kwargs["build_info"].parent / "build/rack_patch_decode"
        executing = kwargs["toolchain"] / "rack_patch_decode"
        candidate.write_bytes(macho(b"same executable code", b"adhoc"))
        executing.write_bytes(
            macho(b"same executable code", b"developer id signature"))
        candidate.chmod(0o755)
        executing.chmod(0o755)
        identity = Q._binary_identity(kwargs["build_info"].parent, candidate)
        stamp = kwargs["toolchain"] / "FORGE_TOOLCHAIN_STAMP"
        stamp.write_text(stamp.read_text() + f"rack_patch_decode {identity}\n")

        identity_path, _inventory_path = Q.prepare_inputs(**kwargs)

        receipt = json.loads(identity_path.read_text())["identity"]
        self.assertEqual(receipt["rack_patch_decode_sha256"], identity)
        self.assertNotEqual(Q._file_digest(candidate), Q._file_digest(executing))

    def test_prepare_rejects_a_decoder_with_mutated_executable_code(self) -> None:
        kwargs = self.prepare_kwargs()
        candidate = kwargs["build_info"].parent / "build/rack_patch_decode"
        executing = kwargs["toolchain"] / "rack_patch_decode"
        candidate.write_bytes(macho(b"candidate executable code", b"signature"))
        executing.write_bytes(macho(b"mutated executable code", b"signature"))
        candidate.chmod(0o755)
        executing.chmod(0o755)

        with self.assertRaisesRegex(Q.QualificationError,
                                    "executing.*does not match"):
            Q.prepare_inputs(**kwargs)
        self.assertFalse(kwargs["output_dir"].exists())

    def test_prepare_requires_explicit_sdk_authority_and_checks_optional_stamp(self) -> None:
        kwargs = self.prepare_kwargs()
        kwargs["expected_pulp_ref"] = "e" * 40
        with self.assertRaisesRegex(Q.QualificationError, "expected Pulp SDK"):
            Q.prepare_inputs(**kwargs)
        self.assertFalse(kwargs["output_dir"].exists())

        kwargs = self.prepare_kwargs()
        with kwargs["build_info"].open("a", encoding="utf-8") as output:
            output.write("expected_pulp_sdk_ref=" + "e" * 40 + "\n")
        with self.assertRaisesRegex(Q.QualificationError, "does not match pulp_sdk"):
            Q.prepare_inputs(**kwargs)
        self.assertFalse(kwargs["output_dir"].exists())

    def test_prepare_preflight_failures_leave_no_partial_pair(self) -> None:
        cases = (
            ("selection", lambda kwargs: kwargs.update(
                selection_loader=lambda _toolchain, _settings: (_ for _ in ()).throw(
                    Q.QualificationError("bad selection")))),
            ("inventory error", lambda kwargs: kwargs.update(
                inventory_loader=lambda _toolchain: (_ for _ in ()).throw(
                    Q.QualificationError("bad inventory")))),
            ("empty inventory", lambda kwargs: kwargs.update(
                inventory_loader=lambda _toolchain: {})),
        )
        for label, mutate in cases:
            with self.subTest(label=label):
                kwargs = self.prepare_kwargs()
                mutate(kwargs)
                with self.assertRaises(Q.QualificationError):
                    Q.prepare_inputs(**kwargs)
                self.assertFalse(kwargs["output_dir"].exists())

    def test_prepare_refuses_partial_or_different_existing_destination(self) -> None:
        kwargs = self.prepare_kwargs()
        kwargs["output_dir"].mkdir()
        kwargs["output_dir"].joinpath("identity.json").write_text("{}\n")
        with self.assertRaisesRegex(Q.QualificationError, "incomplete or owned"):
            Q.prepare_inputs(**kwargs)
        self.assertFalse(kwargs["output_dir"].joinpath("inventory.json").exists())

    def test_prepare_never_clobbers_concurrently_created_empty_directory(self) -> None:
        kwargs = self.prepare_kwargs()
        rename_exclusive = Q._rename_directory_exclusive

        def racing_publish(source: Path, destination: Path) -> None:
            destination.mkdir()
            rename_exclusive(source, destination)

        with mock.patch.object(
                Q, "_rename_directory_exclusive", side_effect=racing_publish):
            with self.assertRaisesRegex(Q.QualificationError, "incomplete or owned"):
                Q.prepare_inputs(**kwargs)
        self.assertTrue(kwargs["output_dir"].is_dir())
        self.assertEqual(list(kwargs["output_dir"].iterdir()), [])
        self.assertEqual(list(self.fixture.root.glob(
            ".qualification-inputs.*.tmp")), [])

    def test_each_scene_gets_one_submission_and_zero_retries(self) -> None:
        runner = FakeRunner()
        code = Q.run_campaign(**self.fixture.kwargs(), runner=runner)
        self.assertEqual(code, 0)
        self.assertEqual(len(runner.commands), 12)
        for scene_id in Q.SCENE_IDS:
            result = json.loads((self.fixture.run_root / scene_id / "result.json").read_text())
            self.assertEqual(result["provider_submissions"], 1)
            self.assertEqual(result["retries"], 0)
            self.assertEqual(result["status"], "generated")

    def test_relative_inputs_are_resolved_before_subprocess_boundary(self) -> None:
        cwd = Path.cwd()
        kwargs = self.fixture.kwargs()
        for key in ("catalogue", "identity", "inventory", "toolchain",
                    "settings", "run_root"):
            kwargs[key] = Path(os.path.relpath(kwargs[key], cwd))
        runner = FakeRunner()
        self.assertEqual(Q.run_campaign(**kwargs, runner=runner), 0)
        self.assertEqual(len(runner.commands), 12)

    def test_resume_is_idempotent_and_keeps_receipt_ownership(self) -> None:
        first = FakeRunner()
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=first), 0)
        claims = {scene_id: (self.fixture.run_root / scene_id / "submission.json").read_bytes()
                  for scene_id in Q.SCENE_IDS}
        second = FakeRunner()
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=second), 0)
        self.assertEqual(second.commands, [])
        for scene_id, receipt in claims.items():
            self.assertEqual(
                (self.fixture.run_root / scene_id / "submission.json").read_bytes(),
                receipt)

    def test_resume_rehashes_required_artifacts_before_accepting_pass(self) -> None:
        first = FakeRunner()
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=first), 0)
        (self.fixture.run_root / "P05-S01" / "artifact.vcv").unlink()
        second = FakeRunner()
        with self.assertRaisesRegex(Q.QualificationError, "missing or changed"):
            Q.run_campaign(**self.fixture.kwargs(), runner=second)
        self.assertEqual(second.commands, [])

    def test_campaign_freezes_settings_used_by_every_scene(self) -> None:
        fixture = self.fixture

        class SettingsMutator(FakeRunner):
            def __call__(self, command, **kwargs):
                result = super().__call__(command, **kwargs)
                fixture.settings.write_text('{"ui_model":"changed"}\n')
                return result

        runner = SettingsMutator()
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=runner), 0)
        frozen = (self.fixture.run_root / "settings.input.json").resolve()
        self.assertEqual(len(runner.commands), 12)
        for command in runner.commands:
            self.assertEqual(command[command.index("--settings") + 1], str(frozen))
        self.assertNotIn("changed", frozen.read_text())

    def test_partial_campaign_rejects_different_frozen_settings_input(self) -> None:
        self.fixture.run_root.mkdir()
        (self.fixture.run_root / "settings.input.json").write_text(
            '{"ui_model":"stale"}\n')
        runner = FakeRunner()
        with self.assertRaisesRegex(Q.QualificationError, "different input bytes"):
            Q.run_campaign(**self.fixture.kwargs(), runner=runner)
        self.assertEqual(runner.commands, [])

    def test_active_scene_owner_cannot_be_reconciled_concurrently(self) -> None:
        fixture = self.fixture
        nested_commands = []

        class ConcurrentProbe(FakeRunner):
            def __call__(self, command, **kwargs):
                nested = FakeRunner()
                with self.assertRaises(Q.QualificationError):
                    Q.run_campaign(**fixture.kwargs(), runner=nested)
                nested_commands.extend(nested.commands)
                return super().__call__(command, **kwargs)

            def assertRaises(self, *args, **kwargs):
                return unittest.TestCase().assertRaises(*args, **kwargs)

        runner = ConcurrentProbe()
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=runner), 0)
        self.assertEqual(nested_commands, [])

    def test_toolchain_drift_after_submission_stops_before_next_scene(self) -> None:
        fixture = self.fixture

        class ToolchainMutator(FakeRunner):
            def __call__(self, command, **kwargs):
                result = super().__call__(command, **kwargs)
                (fixture.toolchain / "patch.py").write_text("# changed\n")
                return result

        runner = ToolchainMutator()
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=runner), 1)
        self.assertEqual(len(runner.commands), 1)
        result = json.loads(
            (self.fixture.run_root / "P05-S01" / "result.json").read_text())
        self.assertFalse(result["toolchain_matches_identity"])

    def test_failure_never_resubmits_and_does_not_skip_later_scenes(self) -> None:
        failed = FakeRunner(returncode=9)
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=failed), 1)
        self.assertEqual(len(failed.commands), 12)
        again = FakeRunner()
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=again), 1)
        self.assertEqual(again.commands, [])
        self.assertTrue((self.fixture.run_root / "P05-S12" / "result.json").is_file())

    def test_failed_scene_run_log_is_rehashed_on_resume(self) -> None:
        failed = FakeRunner(returncode=9)
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=failed), 1)
        log = self.fixture.run_root / "P05-S01" / "run.log"
        log.write_text("tampered\n")
        with self.assertRaisesRegex(Q.QualificationError, "missing or changed"):
            Q.run_campaign(**self.fixture.kwargs(), runner=FakeRunner())

    def test_empty_response_reservation_is_not_a_provider_submission(self) -> None:
        failed = FakeRunner(returncode=9, reserve_empty=True)
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=failed), 1)
        result = json.loads(
            (self.fixture.run_root / "P05-S01" / "result.json").read_text())
        self.assertEqual(result["provider_submissions"], 0)

    def test_zero_exit_with_empty_response_fails_closed(self) -> None:
        runner = FakeRunner(returncode=0, reserve_empty=True)
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=runner), 1)
        result = json.loads(
            (self.fixture.run_root / "P05-S01" / "result.json").read_text())
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["provider_submissions"], 0)

    def test_zero_exit_with_empty_artifact_fails_closed(self) -> None:
        class EmptyArtifact(FakeRunner):
            def __call__(self, command, **kwargs):
                completed = super().__call__(command, **kwargs)
                Path(command[command.index("--out") + 1]).write_text("")
                return completed

        runner = EmptyArtifact()
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=runner), 1)
        result = json.loads(
            (self.fixture.run_root / "P05-S01" / "result.json").read_text())
        self.assertEqual(result["status"], "failed")

    def test_receipt_serialization_failure_never_publishes_final_path(self) -> None:
        receipt = self.fixture.root / "receipt.json"
        with self.assertRaises(TypeError):
            Q._write_exclusive(receipt, {"not_json": {object()}})
        self.assertFalse(receipt.exists())
        self.assertEqual(list(self.fixture.root.glob(".receipt.json.*.tmp")), [])

    def test_claim_without_terminal_receipt_refuses_second_call(self) -> None:
        runner = FakeRunner()
        original = Q._write_exclusive
        writes = 0

        def interrupt_result(path: Path, value: object) -> None:
            nonlocal writes
            writes += 1
            if path.name == "result.json":
                raise KeyboardInterrupt("fixture interruption")
            original(path, value)

        Q._write_exclusive = interrupt_result
        try:
            with self.assertRaises(KeyboardInterrupt):
                Q.run_campaign(**self.fixture.kwargs(), runner=runner)
        finally:
            Q._write_exclusive = original
        self.assertGreater(writes, 0)
        second = FakeRunner()
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=second), 1)
        self.assertEqual(len(second.commands), 11)
        self.assertTrue(all("fixture prompt P05-S01" not in command
                            for command in second.commands))
        reconciled = json.loads(
            (self.fixture.run_root / "P05-S01" / "result.json").read_text())
        self.assertTrue(reconciled["reconciled_interruption"])
        self.assertEqual(reconciled["provider_submissions"], 1)

    def test_operator_cancellation_stops_after_claim_without_second_call(self) -> None:
        commands = []

        def cancelled(command, **_kwargs):
            commands.append(command)
            raise KeyboardInterrupt("fixture cancellation")

        with self.assertRaises(KeyboardInterrupt):
            Q.run_campaign(**self.fixture.kwargs(), runner=cancelled)
        self.assertEqual(len(commands), 1)
        self.assertFalse((self.fixture.run_root / "P05-S02").exists())
        second = FakeRunner()
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=second), 1)
        self.assertEqual(len(second.commands), 11)
        self.assertTrue(all("fixture prompt P05-S01" not in command
                            for command in second.commands))
        reconciled = json.loads(
            (self.fixture.run_root / "P05-S01" / "result.json").read_text())
        self.assertIn("indeterminate", reconciled["error"])

    def test_runner_exception_retains_completed_response_without_second_call(self) -> None:
        def disconnected(command, *, env, **_kwargs):
            attempts = Path(env["FORGE_ATTEMPT_DIR"])
            (attempts / "attempt01-model-response.txt").write_text("response\n")
            Path(env["FORGE_MODEL_PROMPT_RECORD"]).write_text("prompt\n")
            raise RuntimeError("fixture disconnect")

        self.assertEqual(
            Q.run_campaign(**self.fixture.kwargs(), runner=disconnected), 1)
        result = json.loads(
            (self.fixture.run_root / "P05-S01" / "result.json").read_text())
        self.assertEqual(result["provider_submissions"], 1)
        self.assertIn("attempts/attempt01-model-response.txt", result["receipts"])
        second = FakeRunner()
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=second), 1)
        self.assertEqual(second.commands, [])

    def test_missing_identity_or_inventory_fails_before_submission(self) -> None:
        for key in ("identity", "inventory"):
            with self.subTest(key=key):
                kwargs = self.fixture.kwargs()
                kwargs[key] = self.fixture.root / f"missing-{key}.json"
                runner = FakeRunner()
                with self.assertRaisesRegex(Q.QualificationError, "cannot read"):
                    Q.run_campaign(**kwargs, runner=runner)
                self.assertEqual(runner.commands, [])

    def test_mismatched_toolchain_identity_fails_before_submission(self) -> None:
        document = json.loads(self.fixture.identity.read_text())
        document["identity"]["toolchain"]["stamp"] = "different candidate"
        self.fixture.identity.write_text(json.dumps(document))
        runner = FakeRunner()
        with self.assertRaisesRegex(Q.QualificationError, "does not match"):
            Q.run_campaign(**self.fixture.kwargs(), runner=runner)
        self.assertEqual(runner.commands, [])

    def test_mismatched_model_or_live_inventory_fails_before_submission(self) -> None:
        cases = (
            ("selection_loader", lambda _root, _settings: {
                "provider": "codex", "model": "different",
                "reasoning_effort": "medium",
                "codex_executable": str(self.fixture.codex),
                "codex_home": str(self.fixture.codex_home),
            }, "model selection"),
            ("inventory_loader", lambda _root: {"Other": {}}, "Rack inventory"),
        )
        for key, loader, message in cases:
            with self.subTest(key=key):
                kwargs = self.fixture.kwargs()
                kwargs[key] = loader
                runner = FakeRunner()
                with self.assertRaisesRegex(Q.QualificationError, message):
                    Q.run_campaign(**kwargs, runner=runner)
                self.assertEqual(runner.commands, [])

    def test_inventory_drift_after_one_scene_stops_before_another_call(self) -> None:
        snapshots = 0

        def drifting(_root: Path) -> dict:
            nonlocal snapshots
            snapshots += 1
            if snapshots == 1:
                return {"Fixture": {"version": "1", "modules": ["One"]}}
            return {"Changed": {}}

        kwargs = self.fixture.kwargs()
        kwargs["inventory_loader"] = drifting
        runner = FakeRunner()
        self.assertEqual(Q.run_campaign(**kwargs, runner=runner), 1)
        self.assertEqual(len(runner.commands), 1)
        result = json.loads(
            (self.fixture.run_root / "P05-S01" / "result.json").read_text())
        self.assertFalse(result["inventory_matches_receipt"])

    def test_post_submission_validation_error_gets_terminal_receipt(self) -> None:
        snapshots = 0

        def broken_after_call(_root: Path) -> dict:
            nonlocal snapshots
            snapshots += 1
            if snapshots == 1:
                return {"Fixture": {"version": "1", "modules": ["One"]}}
            raise Q.QualificationError("fixture inventory inspection failed")

        kwargs = self.fixture.kwargs()
        kwargs["inventory_loader"] = broken_after_call
        runner = FakeRunner()
        self.assertEqual(Q.run_campaign(**kwargs, runner=runner), 1)
        self.assertEqual(len(runner.commands), 1)
        result_path = self.fixture.run_root / "P05-S01" / "result.json"
        result = json.loads(result_path.read_text())
        self.assertIn("fixture inventory inspection failed",
                      result["post_submission_validation_error"])
        again = FakeRunner()
        self.assertEqual(Q.run_campaign(**self.fixture.kwargs(), runner=again), 1)
        self.assertEqual(again.commands, [])


if __name__ == "__main__":
    unittest.main()
