#!/usr/bin/env python3
"""Provider-free direct tests for shipped-scene adjudication."""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import adjudicate_scenes as A
import qualify_scenes as Q


class Fixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.toolchain = root / "toolchain"
        self.toolchain.mkdir(parents=True)
        for name in (
                "patch.py", "with_app_model_selection.py", "idiom_check.py",
                "fidelity.py", "rack_open.py"):
            (self.toolchain / name).write_text("# fixture\n", encoding="utf-8")
        (self.toolchain / "FORGE_TOOLCHAIN_STAMP").write_text(
            "fixture-forge@abc pulp@def\n", encoding="utf-8")
        self.rack_app = root / "Rack.app"
        (self.rack_app / "Contents/MacOS").mkdir(parents=True)
        (self.rack_app / "Contents/MacOS/Rack").write_bytes(b"fixture Rack binary")
        self.catalogue = root / "catalogue.json"
        self.scenes = [
            {"id": scene_id, "prompt": f"fixture prompt {scene_id}",
             "idiom": f"fixture-idiom-{index:02d}"}
            for index, scene_id in enumerate(Q.SCENE_IDS, 1)
        ]
        self.catalogue.write_text(json.dumps({
            "schema": Q.CATALOGUE_SCHEMA, "scenes": self.scenes,
        }) + "\n", encoding="utf-8")
        self.run_root = root / "run"
        self.run_root.mkdir()
        self.settings = self.run_root / "settings.input.json"
        self.settings.write_text('{"fixture":true}\n', encoding="utf-8")

        toolchain = Q._toolchain_identity(self.toolchain)
        identity = {
            "schema": Q.IDENTITY_SCHEMA,
            "identity": {
                "forge_revision": "abc", "pulp_revision": "def",
                "toolchain": {
                    "stamp": toolchain["stamp"], "files": toolchain["files"],
                },
            },
        }
        inventory = {
            "schema": Q.INVENTORY_SCHEMA,
            "inventory": {
                "Fixture": {"version": "1", "modules": ["One"]},
            },
        }
        Q._write_exclusive(self.run_root / "identity.json", identity)
        Q._write_exclusive(self.run_root / "inventory.json", inventory)
        campaign = {
            "schema": Q.CAMPAIGN_SCHEMA,
            "catalogue_sha256": Q._digest({
                "schema": Q.CATALOGUE_SCHEMA, "scenes": self.scenes,
            }),
            "identity_sha256": Q._digest(identity),
            "inventory_sha256": Q._digest(inventory),
            "toolchain": toolchain,
            "settings_sha256": Q._file_digest(self.settings),
            "model_selection": {"provider": "fixture"},
            "scene_ids": list(Q.SCENE_IDS),
        }
        Q._write_exclusive(self.run_root / "campaign.json", campaign)
        fingerprint = Q._digest(campaign)
        for scene in self.scenes:
            scene_dir = self.run_root / scene["id"]
            attempts = scene_dir / "attempts"
            attempts.mkdir(parents=True)
            files = {
                "attempts/attempt01-model-response.txt": "fixture response\n",
                "model-prompt.txt": "fixture prompt record\n",
                "artifact.vcv": json.dumps({
                    "modules": [{"id": 1, "plugin": "Fixture", "model": "One"}],
                    "cables": [{"id": 1}],
                }) + "\n",
                "artifact.why.json": "{}\n",
                "run.log": "fixture generation log\n",
            }
            for relative, body in files.items():
                path = scene_dir / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(body, encoding="utf-8")
            claim = {
                "schema": "forge.modular.scene_submission_claim.v1",
                "campaign_sha256": fingerprint,
                "scene": scene,
                "maximum_provider_submissions": 1,
                "retries": 0,
            }
            Q._write_exclusive(scene_dir / "submission.json", claim)
            artifact = (scene_dir / "artifact.vcv").resolve()
            result = {
                "schema": Q.RESULT_SCHEMA,
                "scene_id": scene["id"],
                "status": "generated",
                "exit_code": 0,
                "submission_processes": 1,
                "provider_submissions": 1,
                "maximum_provider_submissions": 1,
                "retries": 0,
                "receipts": {
                    relative: Q._file_digest(scene_dir / relative)
                    for relative in files
                },
                "generation": {
                    "status": "generated", "artifact": str(artifact),
                    "idiom": scene["idiom"],
                },
                "follow_on": Q._follow_on(
                    self.toolchain, artifact, scene["idiom"], scene_dir),
            }
            Q._write_exclusive(scene_dir / "result.json", result)

    def context(self) -> A.CampaignContext:
        return A._load_context(self.catalogue, self.run_root)


class FakeChecker:
    def __init__(self, *, unchecked: bool = False, mutation: str = "flip") -> None:
        self.unchecked = unchecked
        self.mutation = mutation

    def load_idioms(self) -> dict:
        return {
            f"fixture-idiom-{index:02d}": {
                "slug": f"fixture-idiom-{index:02d}",
                "topology": [{"id": "audible-path"}],
                "common_mistakes": [{"id": "cut-path", "do": "cut"}],
            }
            for index in range(1, 13)
        }

    def load_roles(self) -> dict:
        return {"roles": {}}

    def check(self, patch, _inventory, _idiom, _roles, unchecked) -> list[str]:
        if self.unchecked and not patch.get("broken"):
            unchecked.append("audible-path: fixture port map is unavailable")
        if patch.get("broken") and self.mutation != "no-flip":
            return ["audible path was cut"]
        return []

    def apply_mistake(self, patch, _mistake, _inventory, _roles):
        if self.mutation == "none":
            return None
        if self.mutation == "no-op":
            return copy.deepcopy(patch)
        broken = copy.deepcopy(patch)
        broken["broken"] = True
        return broken


class FakeRunner:
    def __init__(self, *, fidelity_code: int = 0,
                 crash_name: str | None = None,
                 write_audio: bool = True) -> None:
        self.fidelity_code = fidelity_code
        self.crash_name = crash_name
        self.write_audio = write_audio
        self.commands: list[list[str]] = []
        self.environments: list[dict | None] = []

    def __call__(self, command, *, capture_output, text, timeout, check, env=None):
        self.commands.append(command)
        self.environments.append(env)
        self.test_contract(capture_output, text, timeout, check)
        name = Path(command[1]).name
        if name == self.crash_name:
            raise subprocess.SubprocessError("fixture checker crash")
        code = 0
        if name == "fidelity.py":
            code = self.fidelity_code
            if code in {0, 1} and self.write_audio:
                Path(command[command.index("--wav") + 1]).write_bytes(b"RIFFfixture")
        return subprocess.CompletedProcess(command, code, "fixture stdout\n", "")

    @staticmethod
    def test_contract(capture_output, text, timeout, check) -> None:
        assert capture_output is True
        assert text is True
        assert timeout == 180
        assert check is False


class AdjudicationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.fixture = Fixture(Path(self.temp.name))

    def _one(self, checker: FakeChecker, runner: FakeRunner | None = None) -> dict:
        context = self.fixture.context()
        return A.adjudicate_scene(
            context, context.scenes[0], self.fixture.rack_app,
            runner=runner or FakeRunner(), idiom_loader=lambda _root: checker)

    def test_all_twelve_pass_and_use_exact_rack_argv(self) -> None:
        before = {
            scene["id"]: (self.fixture.run_root / scene["id"] / "result.json").read_bytes()
            for scene in self.fixture.scenes
        }
        runner = FakeRunner()
        code = A.adjudicate_campaign(
            catalogue=self.fixture.catalogue, run_root=self.fixture.run_root,
            rack_app=self.fixture.rack_app, runner=runner,
            idiom_loader=lambda _root: FakeChecker())
        self.assertEqual(code, 0)
        self.assertEqual(len(runner.commands), 36)
        self.assertEqual(
            {Path(command[1]).name for command in runner.commands},
            {"patch.py", "rack_open.py", "fidelity.py"})
        rack_commands = [c for c in runner.commands if Path(c[1]).name == "rack_open.py"]
        self.assertEqual(len(rack_commands), 12)
        for command in rack_commands:
            self.assertEqual(command[2], "--app")
            self.assertEqual(command[4], "--patch")
            self.assertTrue(Path(command[3]).is_absolute())
            self.assertTrue(Path(command[5]).is_absolute())
        rack_binary = str((self.fixture.rack_app / "Contents/MacOS/Rack").resolve())
        for command, environment in zip(runner.commands, runner.environments):
            if Path(command[1]).name == "fidelity.py":
                self.assertEqual(environment["FORGE_RACK_BIN"], rack_binary)
            else:
                self.assertIsNone(environment)
        summary = json.loads(
            (self.fixture.run_root / "qualification-summary.json").read_text())
        self.assertEqual(summary["status"], "PASS")
        self.assertEqual([item["scene_id"] for item in summary["scenes"]],
                         list(Q.SCENE_IDS))
        self.assertEqual(len(summary["scenes"]), 12)
        for scene_id, original in before.items():
            self.assertEqual(
                (self.fixture.run_root / scene_id / "result.json").read_bytes(),
                original)

    def test_tampered_generation_evidence_is_refused_before_commands(self) -> None:
        for label in (
                "catalogue", "campaign", "identity", "inventory",
                "toolchain", "artifact", "result"):
            with self.subTest(label=label):
                isolated = Path(self.temp.name) / f"tamper-{label}"
                fixture = Fixture(isolated)
                path = {
                    "catalogue": fixture.catalogue,
                    "campaign": fixture.run_root / "campaign.json",
                    "identity": fixture.run_root / "identity.json",
                    "inventory": fixture.run_root / "inventory.json",
                    "toolchain": fixture.toolchain / "patch.py",
                    "artifact": fixture.run_root / "P05-S01" / "artifact.vcv",
                    "result": fixture.run_root / "P05-S01" / "result.json",
                }[label]
                path.write_bytes(path.read_bytes() + b"tampered\n")
                runner = FakeRunner()
                with self.assertRaises((A.AdjudicationError, Q.QualificationError)):
                    context = fixture.context()
                    A.adjudicate_scene(
                        context, context.scenes[0], fixture.rack_app,
                        runner=runner, idiom_loader=lambda _root: FakeChecker())
                self.assertEqual(runner.commands, [])

    def test_unchecked_positive_contract_withholds_causal_verdict(self) -> None:
        receipt = self._one(FakeChecker(unchecked=True))
        self.assertEqual(receipt["status"], "WITHHOLD")
        self.assertEqual(receipt["checks"]["idiom_contract"]["status"], "WITHHOLD")
        self.assertTrue(receipt["checks"]["idiom_contract"]["unchecked"])
        self.assertEqual(receipt["checks"]["causal_witness"]["status"], "WITHHOLD")

    def test_first_applicable_mutation_must_materialize(self) -> None:
        receipt = self._one(FakeChecker(mutation="no-op"))
        self.assertEqual(receipt["checks"]["causal_witness"]["status"], "WITHHOLD")
        self.assertIn("did not change", receipt["checks"]["causal_witness"]["error"])
        self.assertEqual(receipt["checks"]["negative_control"]["status"], "WITHHOLD")

    def test_materialized_mutation_must_flip_the_same_checker(self) -> None:
        receipt = self._one(FakeChecker(mutation="no-flip"))
        self.assertEqual(receipt["checks"]["causal_witness"]["status"], "PASS")
        self.assertEqual(receipt["checks"]["negative_control"]["status"], "FAIL")
        self.assertIn("still holds", receipt["checks"]["negative_control"]["error"])

    def test_mutation_proof_records_distinct_hashes_and_finding(self) -> None:
        receipt = self._one(FakeChecker())
        causal = receipt["checks"]["causal_witness"]
        negative = receipt["checks"]["negative_control"]
        self.assertEqual(causal["status"], "PASS")
        self.assertNotEqual(causal["original_sha256"], causal["mutated_sha256"])
        self.assertEqual(negative["status"], "PASS")
        self.assertEqual(negative["findings"], ["audible path was cut"])

    def test_fidelity_unavailable_is_withheld(self) -> None:
        receipt = self._one(FakeChecker(), FakeRunner(fidelity_code=2))
        self.assertEqual(receipt["checks"]["dsp_and_audio"]["status"], "WITHHOLD")
        self.assertEqual(receipt["status"], "WITHHOLD")
        self.assertNotIn("audio", receipt)

    def test_fidelity_verdict_without_wav_is_withheld(self) -> None:
        receipt = self._one(
            FakeChecker(), FakeRunner(fidelity_code=0, write_audio=False))
        self.assertEqual(receipt["checks"]["dsp_and_audio"]["status"], "WITHHOLD")
        self.assertIn("without a non-empty WAV",
                      receipt["checks"]["dsp_and_audio"]["error"])
        self.assertNotIn("audio", receipt)

    def test_checker_crash_is_withheld(self) -> None:
        receipt = self._one(
            FakeChecker(), FakeRunner(crash_name="rack_open.py"))
        self.assertEqual(receipt["checks"]["rack_load"]["status"], "WITHHOLD")
        self.assertIn("could not run", receipt["checks"]["rack_load"]["error"])
        self.assertEqual(receipt["status"], "WITHHOLD")

    def test_resume_reuses_exclusive_receipts_and_summary(self) -> None:
        first = FakeRunner()
        kwargs = dict(
            catalogue=self.fixture.catalogue, run_root=self.fixture.run_root,
            rack_app=self.fixture.rack_app,
            idiom_loader=lambda _root: FakeChecker())
        self.assertEqual(A.adjudicate_campaign(**kwargs, runner=first), 0)
        adjudications = {
            scene_id: (self.fixture.run_root / scene_id / "adjudication.json").read_bytes()
            for scene_id in Q.SCENE_IDS
        }
        summary = (self.fixture.run_root / "qualification-summary.json").read_bytes()
        second = FakeRunner()
        self.assertEqual(A.adjudicate_campaign(**kwargs, runner=second), 0)
        self.assertEqual(second.commands, [])
        self.assertEqual(
            (self.fixture.run_root / "qualification-summary.json").read_bytes(), summary)
        for scene_id, receipt in adjudications.items():
            self.assertEqual(
                (self.fixture.run_root / scene_id / "adjudication.json").read_bytes(),
                receipt)

    def test_resume_refuses_a_different_rack_installation(self) -> None:
        context = self.fixture.context()
        A.adjudicate_scene(
            context, context.scenes[0], self.fixture.rack_app,
            runner=FakeRunner(), idiom_loader=lambda _root: FakeChecker())
        other_app = self.fixture.root / "Other Rack.app"
        (other_app / "Contents/MacOS").mkdir(parents=True)
        (other_app / "Contents/MacOS/Rack").write_bytes(b"other Rack binary")
        runner = FakeRunner()
        with self.assertRaisesRegex(A.AdjudicationError, "evidence changed"):
            A.adjudicate_scene(
                context, context.scenes[0], other_app,
                runner=runner, idiom_loader=lambda _root: FakeChecker())
        self.assertEqual(runner.commands, [])

    def test_resume_refuses_changed_installed_plugin_bytes(self) -> None:
        plugin_root = self.fixture.root / "plugins-mac-arm64"
        plugin = plugin_root / "Fixture"
        plugin.mkdir(parents=True)
        binary = plugin / "plugin.dylib"
        binary.write_bytes(b"fixture plugin binary")
        original_stat = binary.stat()
        context = self.fixture.context()
        with mock.patch.dict("os.environ", {"RACK_PLUGIN_DIR": str(plugin_root)}):
            A.adjudicate_scene(
                context, context.scenes[0], self.fixture.rack_app,
                runner=FakeRunner(), idiom_loader=lambda _root: FakeChecker())
            binary.write_bytes(b"tampered plugin bytes")
            os.utime(binary, ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns))
            self.assertEqual(binary.stat().st_ino, original_stat.st_ino)
            self.assertEqual(binary.stat().st_size, original_stat.st_size)
            self.assertEqual(binary.stat().st_mtime_ns, original_stat.st_mtime_ns)
            runner = FakeRunner()
            with self.assertRaisesRegex(A.AdjudicationError, "evidence changed"):
                A.adjudicate_scene(
                    context, context.scenes[0], self.fixture.rack_app,
                    runner=runner, idiom_loader=lambda _root: FakeChecker())
            self.assertEqual(runner.commands, [])

    def test_resume_refuses_terminal_dsp_verdict_without_audio(self) -> None:
        context = self.fixture.context()
        A.adjudicate_scene(
            context, context.scenes[0], self.fixture.rack_app,
            runner=FakeRunner(), idiom_loader=lambda _root: FakeChecker())
        path = self.fixture.run_root / "P05-S01" / "adjudication.json"
        receipt = json.loads(path.read_text())
        del receipt["audio"]
        path.write_text(json.dumps(receipt) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(A.AdjudicationError, "no preserved WAV"):
            A.adjudicate_scene(
                context, context.scenes[0], self.fixture.rack_app,
                runner=FakeRunner(), idiom_loader=lambda _root: FakeChecker())

    def test_resume_refuses_edited_check_classifications(self) -> None:
        for check_name in A.CHECK_NAMES:
            with self.subTest(check_name=check_name):
                fixture = Fixture(Path(self.temp.name) / f"tamper-{check_name}")
                context = fixture.context()
                A.adjudicate_scene(
                    context, context.scenes[0], fixture.rack_app,
                    runner=FakeRunner(), idiom_loader=lambda _root: FakeChecker())
                path = fixture.run_root / "P05-S01" / "adjudication.json"
                receipt = json.loads(path.read_text())
                receipt["checks"][check_name]["status"] = "FAIL"
                receipt["status"] = A._status(receipt["checks"])
                path.write_text(json.dumps(receipt) + "\n", encoding="utf-8")
                runner = FakeRunner()
                with self.assertRaisesRegex(
                        A.AdjudicationError,
                        f"{check_name} classification does not match its evidence"):
                    A.adjudicate_scene(
                        context, context.scenes[0], fixture.rack_app,
                        runner=runner, idiom_loader=lambda _root: FakeChecker())
                self.assertEqual(runner.commands, [])

    def test_summary_refuses_failed_receipts_edited_to_pass(self) -> None:
        self.assertEqual(A.adjudicate_campaign(
            catalogue=self.fixture.catalogue, run_root=self.fixture.run_root,
            rack_app=self.fixture.rack_app, runner=FakeRunner(fidelity_code=1),
            idiom_loader=lambda _root: FakeChecker()), 1)
        (self.fixture.run_root / "qualification-summary.json").unlink()
        for scene_id in Q.SCENE_IDS:
            path = self.fixture.run_root / scene_id / "adjudication.json"
            receipt = json.loads(path.read_text())
            receipt["checks"]["dsp_and_audio"]["status"] = "PASS"
            receipt["status"] = "PASS"
            path.write_text(json.dumps(receipt) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(
                A.AdjudicationError,
                "dsp_and_audio classification does not match its evidence"):
            A.write_summary(
                self.fixture.context(), self.fixture.rack_app)

    def test_summary_refuses_failed_command_evidence_edited_to_pass(self) -> None:
        self.assertEqual(A.adjudicate_campaign(
            catalogue=self.fixture.catalogue, run_root=self.fixture.run_root,
            rack_app=self.fixture.rack_app, runner=FakeRunner(fidelity_code=1),
            idiom_loader=lambda _root: FakeChecker()), 1)
        (self.fixture.run_root / "qualification-summary.json").unlink()
        for scene_id in Q.SCENE_IDS:
            path = self.fixture.run_root / scene_id / "adjudication.json"
            receipt = json.loads(path.read_text())
            receipt["checks"]["dsp_and_audio"]["exit_code"] = 0
            receipt["checks"]["dsp_and_audio"]["status"] = "PASS"
            receipt["status"] = "PASS"
            path.write_text(json.dumps(receipt) + "\n", encoding="utf-8")
        with self.assertRaisesRegex(
                A.AdjudicationError, "recorded check evidence changed"):
            A.write_summary(self.fixture.context(), self.fixture.rack_app)

    def test_interrupted_receipt_publication_can_rerun_fidelity(self) -> None:
        context = self.fixture.context()
        with mock.patch.object(
                A.Q, "_write_exclusive",
                side_effect=OSError("fixture publication interruption")):
            with self.assertRaisesRegex(OSError, "publication interruption"):
                A.adjudicate_scene(
                    context, context.scenes[0], self.fixture.rack_app,
                    runner=FakeRunner(), idiom_loader=lambda _root: FakeChecker())
        self.assertFalse(
            (self.fixture.run_root / "P05-S01" / "adjudication.json").exists())
        orphaned = list((self.fixture.run_root / "P05-S01").glob("audio-*.wav"))
        self.assertEqual(len(orphaned), 1)
        runner = FakeRunner()
        receipt = A.adjudicate_scene(
            context, context.scenes[0], self.fixture.rack_app,
            runner=runner, idiom_loader=lambda _root: FakeChecker())
        self.assertEqual(receipt["status"], "PASS")
        self.assertEqual(len(runner.commands), 3)
        self.assertEqual(receipt["audio"]["path"], orphaned[0].name)

    def test_summary_refuses_eleven_of_twelve_receipts(self) -> None:
        runner = FakeRunner()
        self.assertEqual(A.adjudicate_campaign(
            catalogue=self.fixture.catalogue, run_root=self.fixture.run_root,
            rack_app=self.fixture.rack_app, runner=runner,
            idiom_loader=lambda _root: FakeChecker()), 0)
        (self.fixture.run_root / "qualification-summary.json").unlink()
        (self.fixture.run_root / "P05-S12" / "adjudication.json").unlink()
        with self.assertRaisesRegex(A.AdjudicationError, "missing P05-S12"):
            A.write_summary(self.fixture.context(), self.fixture.rack_app)


if __name__ == "__main__":
    unittest.main()
