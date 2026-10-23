#!/usr/bin/env python3
"""Self-test for real_host_smoke.py: every step except the host itself.

No DAW is launched. The process list, the launch and the plug-in are
simulated: the plug-in's run directory is filled from WP-h1's synthesize_run,
shaped as the claiming instance would write it, so the receipt the driver
builds goes through the real v2 verifier.
"""
from __future__ import annotations

import json
import os
import plistlib
import shutil
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import real_host_smoke as rhs  # noqa: E402

gfc, hosts = rhs.gfc, rhs.hosts
LOGIC = rhs.HOSTS["logic"]["executable"]
FAULT, POST, FRAMES = 3000, 48000, 9600
QUIET = {"status": "ok", "quiet_host": True, "reasons": []}
UNARMED = {"input": "No Input", "record_armed": False, "input_monitoring": False}


class FakeDefaults:
    """`defaults` over an in-memory store, with `import` merging the way the
    real one does (keys missing from the file are kept)."""

    def __init__(self):
        self.domains: dict[str, dict] = {}
        self.calls: list[list[str]] = []
        self.import_works = True

    def __call__(self, args: list[str]) -> None:
        self.calls.append(list(args))
        verb, domain = args[0], args[1]
        if verb == "export":
            Path(args[2]).write_bytes(plistlib.dumps(self.domains.get(domain, {}),
                                                     fmt=plistlib.FMT_BINARY))
        elif verb == "import":
            if self.import_works:
                self.domains.setdefault(domain, {}).update(
                    plistlib.loads(Path(args[2]).read_bytes()))
        elif verb == "delete":
            del self.domains[domain][args[2]]
        else:
            raise AssertionError(f"unexpected defaults verb {verb}")


class Env(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        base = Path(self._tmp.name)
        self.runs = base / "runs"
        self.run = self.runs / "run-1"
        self.root = base / "rendezvous"
        self.components = base / "Components"
        self.components.mkdir(parents=True)
        self._env = {k: os.environ.get(k) for k in ("PULP_GPU_FAULT_RENDEZVOUS_ROOT",
                                                    "PULP_REAL_HOST_COMPONENTS_DIR")}
        os.environ["PULP_GPU_FAULT_RENDEZVOUS_ROOT"] = str(self.root)
        os.environ["PULP_REAL_HOST_COMPONENTS_DIR"] = str(self.components)
        self.procs: list[tuple[int, str]] = []
        self.locked = False
        # No test may reach this user's real preference domains.
        self.prefs = FakeDefaults()
        real, rhs.defaults = rhs.defaults, self.prefs
        self.addCleanup(setattr, rhs, "defaults", real)

    def tearDown(self):
        for key, value in self._env.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        self._tmp.cleanup()

    def preflight(self, host="logic"):
        return rhs.preflight(host, self.run, runs_parent=self.runs,
                             processes=lambda: list(self.procs),
                             host_preflight=lambda: dict(QUIET),
                             locked=lambda: self.locked)


class Preflight(Env):
    def test_a_clean_machine_passes(self):
        ok, reasons = self.preflight()
        self.assertTrue(ok, reasons)
        self.assertTrue((self.run / "preflight.json").is_file())

    def test_a_running_daw_refuses_the_run(self):
        for exe in rhs.OTHER_DAW_EXECUTABLES:
            with self.subTest(exe=exe):
                self.procs = [(4242, exe)]
                ok, reasons = self.preflight()
                self.assertFalse(ok)
                self.assertIn("already running", reasons[0])

    def test_a_running_bitwig_engine_refuses_a_bitwig_run(self):
        self.procs = [(77, rhs.HOSTS["bitwig"]["engine_prefix"] + " ARM64-NEON.app/x")]
        ok, _ = self.preflight("bitwig")
        self.assertFalse(ok)

    def test_a_locked_console_refuses_the_run(self):
        self.locked = True
        ok, reasons = self.preflight()
        self.assertFalse(ok)
        self.assertIn("console is locked", reasons[0])

    def test_a_stale_rendezvous_refuses_the_run(self):
        self.root.mkdir(parents=True)
        (self.root / "active.json").write_text("{}", encoding="utf-8")
        ok, reasons = self.preflight()
        self.assertFalse(ok)
        self.assertIn("rendezvous is already active", reasons[0])

    def test_an_unreverted_earlier_run_refuses_the_next(self):
        earlier = self.runs / "run-0"
        earlier.mkdir(parents=True)
        rhs.record_change(earlier, "rendezvous_run", path=str(self.root / "runs" / "x"))
        ok, reasons = self.preflight()
        self.assertFalse(ok)
        self.assertIn("never reverted", reasons[0])
        rhs.revert(earlier)
        self.assertTrue(self.preflight()[0])

    def test_stage_refuses_after_a_refused_preflight(self):
        self.procs = [(1, LOGIC)]
        self.preflight()
        with self.assertRaises(SystemExit):
            rhs.stage(self.run, fault="force_loss_before", at_sample=FAULT, window_post=POST,
                      stimulus_frames=FRAMES)


class Stage(Env):
    def staged(self, **kw):
        self.assertTrue(self.preflight()[0])
        args = dict(fault="force_loss_before", at_sample=FAULT, window_post=POST,
                    stimulus_frames=FRAMES)
        args.update(kw)
        return rhs.stage(self.run, **args)

    def test_the_rendezvous_is_what_the_plugin_accepts(self):
        st = self.staged()
        active = json.loads((self.root / "active.json").read_text(encoding="utf-8"))
        self.assertEqual(active["schema"], rhs.RENDEZVOUS_SCHEMA)
        self.assertEqual(active["nonce"], st["nonce"])
        self.assertRegex(active["nonce"], r"^[0-9a-f]{32}$")
        run_dir = Path(active["run_dir"])
        self.assertEqual(run_dir.parent, self.root / "runs")
        for key in ("request", "ir"):
            self.assertEqual(Path(active[key]).parent, run_dir)
            self.assertTrue(Path(active[key]).is_file())
        # Within the plug-in's one-hour ceiling, long enough for AU validation.
        self.assertLessEqual(active["expires_unix"] - time.time(), 3600)
        self.assertGreaterEqual(active["expires_unix"] - time.time(), 600)
        request = json.loads(Path(active["request"]).read_text(encoding="utf-8"))
        self.assertEqual(request["after_project_sample"], FAULT + POST)

    def test_staged_and_internal_options_reach_the_rendezvous(self):
        self.staged(provider="staged", input_mode="internal", fault="positive_control",
                    planted="gpu_off")
        active = json.loads((self.root / "active.json").read_text(encoding="utf-8"))
        self.assertEqual(active["provider"], "staged")
        self.assertEqual(active["stimulus"], "internal")
        st = rhs.state(self.run)
        self.assertEqual(st["control_kind"], "planted_negative")

    def test_a_live_template_requires_the_internal_stimulus(self):
        with self.assertRaises(SystemExit):
            self.staged(template_kind="live", input_mode="host")

    def test_a_format_the_host_cannot_load_is_refused(self):
        with self.assertRaises(SystemExit):
            self.staged(fmt="vst3")

    def test_the_template_is_copied_never_opened_in_place(self):
        template = Path(self._tmp.name) / "Template.logicx"
        (template / "Alternatives").mkdir(parents=True)
        (template / "Alternatives" / "ProjectData").write_bytes(b"x")
        rhs.write_template_manifest(template, host_id="logic", kind="playback")
        st = self.staged(template=template)
        self.assertNotEqual(Path(st["project"]).parent, template.parent)
        self.assertTrue((Path(st["project"]) / "Alternatives" / "ProjectData").is_file())
        cmd = rhs.launch_command(self.run)
        self.assertEqual(cmd[:3], ["open", "-g", "-n"])
        self.assertIn(st["project"], cmd)
        self.assertNotIn(str(template), cmd)
        self.assertIn("-ApplePersistenceIgnoreState", cmd)
        self.assertIn("--arch", rhs.launch_command(self.run, rosetta=True))

    def test_a_template_without_or_unlike_its_manifest_is_refused(self):
        template = Path(self._tmp.name) / "T.logicx"
        template.mkdir()
        (template / "ProjectData").write_bytes(b"reviewed")
        self.assertTrue(self.preflight()[0])
        args = dict(fault="force_loss_before", at_sample=FAULT, window_post=POST,
                    stimulus_frames=FRAMES, template=template)
        with self.assertRaisesRegex(SystemExit, "no reviewed manifest"):
            rhs.stage(self.run, **args)
        rhs.write_template_manifest(template, host_id="logic", kind="live")
        with self.assertRaisesRegex(SystemExit, "logic/live template"):
            rhs.stage(self.run, **args)
        rhs.write_template_manifest(template, host_id="logic", kind="playback")
        (template / "ProjectData").write_bytes(b"edited after review")
        with self.assertRaisesRegex(SystemExit, "changed since"):
            rhs.stage(self.run, **args)

    def test_await_claim_removes_the_rendezvous_at_once(self):
        st = self.staged()
        (Path(st["plugin_run"]) / "claim").write_text("1\n2\n", encoding="utf-8")
        rhs.await_claim(self.run, timeout=1, poll=0.01)
        self.assertFalse((self.root / "active.json").exists())
        self.assertTrue(any(e["kind"] == "rendezvous_active" and e["reverted"]
                            for e in json.loads((self.run / "revert_log.json")
                                                .read_text(encoding="utf-8"))))

    def test_no_claim_is_inconclusive_and_still_removes_the_rendezvous(self):
        self.staged()
        with self.assertRaises(SystemExit):
            rhs.await_claim(self.run, timeout=0.05, poll=0.01)
        self.assertFalse((self.root / "active.json").exists())


class Templates(unittest.TestCase):
    def setUp(self):
        self.spec = json.loads(rhs.TEMPLATES.read_text(encoding="utf-8"))
        self.tmp = Path(tempfile.mkdtemp(prefix="wpk-save-as-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_no_template_kind_arms_the_track(self):
        # Logic refuses to arm a No Input track, and a live row does not need
        # a recording: every kind plays an unarmed track with monitoring off.
        for kind, spec in self.spec["kinds"].items():
            with self.subTest(kind):
                self.assertIs(spec["record_armed"], False)
                self.assertIs(spec["input_monitoring"], False)
        self.assertTrue(self.spec["kinds"]["live"]["plugin_input"].startswith("internal"))

    def test_each_host_template_uses_a_label_the_verifier_accepts(self):
        self.assertEqual(set(self.spec["hosts"]), set(hosts.TEMPLATE_HOSTS))
        for host_id, spec in self.spec["hosts"].items():
            with self.subTest(host_id):
                track = dict(UNARMED, input=spec["no_input_label"])
                self.assertEqual(hosts.track_state_problem(track, host_id), "")

    def test_the_save_as_plan_fronts_only_for_the_key_commands(self):
        plan = rhs.save_as_plan("logic", self.tmp / "WPK-live.logicx")
        actions = [s["action"] for s in plan["steps"]]
        self.assertEqual(actions, ["hotkey", "await_window", "hotkey", "type_text", "press_key",
                                   "type_text", "press_key", "verify_exists"])
        self.assertEqual(plan["steps"][0]["keys"], ["cmd", "shift", "s"])
        self.assertEqual(plan["steps"][2]["keys"], ["cmd", "shift", "g"])
        self.assertEqual([s.get("delivery_mode") for s in plan["steps"]],
                         ["foreground", None, "foreground", None, None, None, None, None])
        self.assertEqual(plan["steps"][3]["text"], os.path.realpath(self.tmp))
        self.assertEqual(plan["steps"][5]["text"], "WPK-live")

    def test_the_save_as_plan_refuses_unsafe_targets(self):
        existing = self.tmp / "WPK-playback.logicx"
        existing.mkdir()
        for target, needle in (
                (Path.home() / "Music" / "Logic" / "WPK-live.logicx", "temp directory"),
                (self.tmp / "WPK-live.band", "ends in .logicx"),
                (existing, "never overwritten"),
                (self.tmp / "missing" / "WPK-live.logicx", "does not exist")):
            with self.subTest(needle):
                with self.assertRaisesRegex(SystemExit, needle):
                    rhs.save_as_plan("logic", target)

    def test_the_save_as_cli_prints_the_plan_or_refuses(self):
        self.assertEqual(rhs.main(["save-as-plan", "--host", "bitwig", "--target",
                                   str(self.tmp / "WPK-live.bwproject")]), rhs.EXIT_OK)
        self.assertEqual(rhs.main(["save-as-plan", "--host", "bitwig", "--target",
                                   str(Path.home() / "WPK-live.bwproject")]), rhs.EXIT_USAGE)


class WrapReaper(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="wpk-wrap-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_the_launch_bound_is_the_preflight_sample(self):
        doc = {"observations": [
            {"argv": ["/bin/ps"], "stdout": "1 0.0 launchd"},
            {"argv": ["/repo/tools/scripts/host_vitals.sh", "--json"],
             "stdout": json.dumps({"level": "green", "sampled_at": 1792769309})}]}
        self.assertEqual(rhs._preflight_sampled_us(doc), 1792769309 * 1_000_000)
        for broken in ({}, {"observations": [{"argv": ["/x/host_vitals.sh"], "stdout": "{"}]},
                       {"observations": [{"argv": ["/x/host_vitals.sh"], "stdout": "{}"}]}):
            with self.subTest(broken=broken):
                self.assertIsNone(rhs._preflight_sampled_us(broken))

    def test_the_control_kind_comes_from_what_the_plugin_armed(self):
        none = {"planted_negative": None, "gpu_off": False}
        self.assertEqual(rhs.reaper_control_kind(dict(none, planted_negative="no_fallback"),
                                                 "force_loss_after"), "planted_negative")
        self.assertEqual(rhs.reaper_control_kind(dict(none, gpu_off=True), "positive_control"),
                         "planted_negative")
        self.assertEqual(rhs.reaper_control_kind(none, "positive_control"), "positive_control")
        self.assertEqual(rhs.reaper_control_kind(none, "worker_stall"), "fault")

    def test_only_a_run_v1_verifies_live_is_wrapped(self):
        # A dry run verifies as v1 only with --allow-dry-run; it is not DAW
        # evidence, so it never becomes a v2 receipt.
        run = self.tmp / "dry"
        proc = rhs.subprocess.run(
            [sys.executable, str(HERE / "reaper_smoke.py"), "--mode", "gpu-fault",
             "--gpu-fault", "force_loss_after", "--dry-run", "--format", "clap",
             "--plugin-name", "x", "--plugin-path", "/nonexistent", "--out-dir", str(run)],
            capture_output=True, text=True, check=False)
        self.assertTrue((run / "receipt.json").is_file(), proc.stdout[-2000:])
        with self.assertRaisesRegex(rhs.V2.ReceiptError, "not DAW evidence"):
            rhs.wrap_reaper(run, self.tmp / "out", code_commit="c" * 40)
        self.assertFalse((self.tmp / "out").exists())
        self.assertEqual(rhs.main(["wrap-reaper", "--reaper-run", str(run), "--out",
                                   str(self.tmp / "out"), "--code-commit", "c" * 40]),
                         rhs.EXIT_USAGE)


class Revert(Env):
    def test_every_change_is_reverted_and_reverting_twice_is_safe(self):
        self.assertTrue(self.preflight()[0])
        component = Path(self._tmp.name) / "Pulp GPU Fault Continuity.component"
        (component / "Contents").mkdir(parents=True)
        installed = rhs.install_au(self.run, component)
        template = Path(self._tmp.name) / "T.logicx"
        template.mkdir()
        rhs.write_template_manifest(template, host_id="logic", kind="playback")
        st = rhs.stage(self.run, fault="force_loss_before", at_sample=FAULT, window_post=POST,
                       stimulus_frames=FRAMES, template=template)
        self.assertTrue(installed.exists())
        self.assertEqual(len(rhs.unreverted(self.run)), 4)
        rhs.revert(self.run)
        self.assertFalse(installed.exists())
        self.assertFalse(Path(st["plugin_run"]).exists())
        self.assertFalse((self.root / "active.json").exists())
        self.assertFalse(Path(st["project"]).parent.exists())
        self.assertEqual(rhs.unreverted(self.run), [])
        rhs.revert(self.run)

    def test_prefs_are_restored_byte_for_byte(self):
        prefs = Path(self._tmp.name) / "prefs"
        prefs.mkdir()
        (prefs / "a.prefs").write_bytes(b"original")
        saved = rhs.HOSTS["bitwig"]["prefs_dir"]
        rhs.HOSTS["bitwig"]["prefs_dir"] = str(prefs)
        try:
            self.assertTrue(self.preflight("bitwig")[0])
            rhs.snapshot_prefs(self.run, "bitwig")
            (prefs / "a.prefs").write_bytes(b"changed by the run")
            (prefs / "new.prefs").write_bytes(b"x")
            rhs.revert(self.run)
            self.assertEqual((prefs / "a.prefs").read_bytes(), b"original")
            self.assertFalse((prefs / "new.prefs").exists())
        finally:
            rhs.HOSTS["bitwig"]["prefs_dir"] = saved

    def logic_run_that_rewrites_prefs(self) -> dict:
        original = {"recent": "~/Music/Logic/Owner.logicx", "zoom": 150, "kept": [1, 2]}
        self.prefs.domains["com.apple.logic10"] = dict(original)
        self.assertTrue(self.preflight()[0])
        rhs.snapshot_prefs(self.run, "logic")
        # What one launch did in the live window: a recent project, a
        # changed value, a new key and (here) a removed one.
        logic = self.prefs.domains["com.apple.logic10"]
        logic.update(recent="EmptySong", zoom=100, NewTrackSettingsV1={"Audio": {"In": -1}})
        del logic["kept"]
        return original

    def test_logic_prefs_are_restored_including_keys_the_launch_added(self):
        original = self.logic_run_that_rewrites_prefs()
        rhs.revert(self.run)
        self.assertEqual(self.prefs.domains["com.apple.logic10"], original)
        self.assertIn(["delete", "com.apple.logic10", "NewTrackSettingsV1"], self.prefs.calls)
        self.assertEqual(rhs.unreverted(self.run), [])

    def test_unchanged_prefs_are_not_imported(self):
        self.prefs.domains["com.apple.logic10"] = {"zoom": 150}
        self.assertTrue(self.preflight()[0])
        rhs.snapshot_prefs(self.run, "logic")
        rhs.revert(self.run)
        self.assertEqual([c[0] for c in self.prefs.calls], ["export", "export"])

    def test_a_restore_that_does_not_match_stays_unreverted(self):
        self.logic_run_that_rewrites_prefs()
        self.prefs.import_works = False
        with self.assertRaisesRegex(RuntimeError, "REFUSING TO MARK com.apple.logic10"):
            rhs.revert(self.run)
        self.assertEqual([e["kind"] for e in rhs.unreverted(self.run)],
                         ["prefs_domain_snapshot"])
        ok, reasons = rhs.preflight("logic", self.runs / "run-2", runs_parent=self.runs,
                                    processes=lambda: [], host_preflight=lambda: dict(QUIET),
                                    locked=lambda: False)
        self.assertFalse(ok, reasons)

    def test_a_missing_export_leaves_the_prefs_untouched(self):
        self.logic_run_that_rewrites_prefs()
        shutil.rmtree(self.run / "prefs-before")
        with self.assertRaisesRegex(RuntimeError, "saved export .* is missing"):
            rhs.revert(self.run)
        self.assertNotIn("import", [c[0] for c in self.prefs.calls])

    def test_garageband_exports_its_own_domain(self):
        self.assertTrue(self.preflight("garageband")[0])
        saved = rhs.snapshot_prefs(self.run, "garageband")
        self.assertEqual(self.prefs.calls,
                         [["export", "com.apple.garageband10",
                           str(saved / "com.apple.garageband10.plist")]])


class Safety(Env):
    """Each guard that protects the owner's machine, with the case it stops."""

    def bitwig_prefs(self):
        prefs = Path(self._tmp.name) / "prefs"
        prefs.mkdir()
        (prefs / "a.prefs").write_bytes(b"original")
        saved = rhs.HOSTS["bitwig"]["prefs_dir"]
        rhs.HOSTS["bitwig"]["prefs_dir"] = str(prefs)
        self.addCleanup(rhs.HOSTS["bitwig"].__setitem__, "prefs_dir", saved)
        self.assertTrue(self.preflight("bitwig")[0])
        return prefs

    def test_a_second_prefs_snapshot_is_refused(self):
        prefs = self.bitwig_prefs()
        rhs.snapshot_prefs(self.run, "bitwig")
        (prefs / "a.prefs").write_bytes(b"changed by the run")
        with self.assertRaisesRegex(SystemExit, "already holds a prefs snapshot"):
            rhs.snapshot_prefs(self.run, "bitwig")
        rhs.revert(self.run)
        self.assertEqual((prefs / "a.prefs").read_bytes(), b"original")

    def test_a_missing_saved_copy_leaves_the_prefs_untouched(self):
        prefs = self.bitwig_prefs()
        rhs.snapshot_prefs(self.run, "bitwig")
        (prefs / "a.prefs").write_bytes(b"current")
        shutil.rmtree(self.run / "prefs-before")
        with self.assertRaisesRegex(RuntimeError, "saved copy .* is missing"):
            rhs.revert(self.run)
        self.assertEqual((prefs / "a.prefs").read_bytes(), b"current")
        self.assertEqual(len(rhs.unreverted(self.run)), 1)

    def test_a_rendezvous_that_appeared_since_the_preflight_is_not_overwritten(self):
        self.assertTrue(self.preflight()[0])
        self.root.mkdir(parents=True, exist_ok=True)
        (self.root / "active.json").write_text('{"nonce": "someone-else"}', encoding="utf-8")
        with self.assertRaisesRegex(SystemExit, "another rendezvous appeared"):
            rhs.stage(self.run, fault="force_loss_before", at_sample=FAULT, window_post=POST,
                      stimulus_frames=FRAMES)
        rhs.revert(self.run)
        self.assertEqual(json.loads((self.root / "active.json").read_text(encoding="utf-8")),
                         {"nonce": "someone-else"})

    def test_another_runs_rendezvous_is_never_deleted(self):
        self.assertTrue(self.preflight()[0])
        st = rhs.stage(self.run, fault="force_loss_before", at_sample=FAULT, window_post=POST,
                       stimulus_frames=FRAMES)
        # Another run replaced it (after this one's rendezvous was lost).
        (self.root / "active.json").write_text('{"nonce": "someone-else"}', encoding="utf-8")
        (Path(st["plugin_run"]) / "claim").write_text("1\n2\n", encoding="utf-8")
        rhs.await_claim(self.run, timeout=1, poll=0.01)
        self.assertTrue((self.root / "active.json").exists())
        entries = rhs.revert(self.run)
        self.assertTrue((self.root / "active.json").exists())
        self.assertFalse(Path(st["plugin_run"]).exists())
        self.assertTrue(any(e.get("note", "").startswith("left in place") for e in entries))

    def test_logic_is_not_launched_before_its_prefs_are_saved(self):
        self.assertTrue(self.preflight()[0])
        rhs.stage(self.run, fault="force_loss_before", at_sample=FAULT, window_post=POST,
                  stimulus_frames=FRAMES)
        launched: list[list[str]] = []
        with self.assertRaisesRegex(SystemExit, "snapshot-prefs"):
            rhs.launch(self.run, processes=lambda: [], execute=launched.append, timeout=0.05)
        self.assertEqual(launched, [])
        rhs.snapshot_prefs(self.run, "logic")
        with self.assertRaisesRegex(SystemExit, "already holds a prefs snapshot"):
            rhs.snapshot_prefs(self.run, "logic")

    def test_no_revert_while_the_host_is_running(self):
        self.assertTrue(self.preflight()[0])
        component = Path(self._tmp.name) / "X.component"
        (component / "Contents").mkdir(parents=True)
        installed = rhs.install_au(self.run, component)
        rhs.save_state(self.run, host_pid=4242, engine_pids=[])
        alive = [(4242, LOGIC)]
        with self.assertRaisesRegex(RuntimeError, "REFUSING TO REVERT"):
            rhs.revert(self.run, processes=lambda: alive, wait_s=0.05, poll=0.01)
        self.assertTrue(installed.exists())
        rhs.revert(self.run, processes=lambda: [], wait_s=0.05, poll=0.01)
        self.assertFalse(installed.exists())


class EndToEnd(Env):
    """Stage, a simulated launch and plug-in, the realtime render's
    provenance, collect: the receipt the driver writes verifies."""

    def simulate_plugin(self, st: dict, host_pid: int) -> None:
        plugin_run = Path(st["plugin_run"])
        with tempfile.TemporaryDirectory() as tmp:
            synth = Path(tmp)
            gfc.synthesize_run(synth, fault=st["fault"], fault_sample=FAULT, frames=FRAMES,
                               nonce=st["nonce"], pid=host_pid, tail=POST, window_post=POST)
            for name in ("stimulus.wav", "ir.f32", "capture.bin", "events.jsonl",
                         "bounce.wav", "realtime.wav"):
                shutil.copyfile(synth / name, plugin_run / name)
        claimed = rhs.now_us()
        (plugin_run / "claim").write_text(f"{host_pid}\n{claimed}\n", encoding="utf-8")
        events = gfc.load_events(plugin_run / "events.jsonl")
        proc = {"event": "host_process", "pid": host_pid, "ppid": 1, "responsible_pid": -1,
                "responsible_path": "", "sandboxed": 0,
                "start_unix_us": rhs.state(self.run)["launch_unix_us"] + 1,
                "claimed_unix_us": claimed, "process_path": LOGIC,
                "config_source": "rendezvous", "nonce": st["nonce"]}
        for event in events:
            # As the Batch A plug-in reports them: the interposer's application
            # time on the capture clock (G3) and the loss named in the status (G2).
            if event["event"] == "provider_fault_applied":
                event["host_time_ns"] = (event["project_sample"] + 512) * 20833
            if event["event"] == "health" and event["phase"] == "after":
                event["status"].update(provider_state="unavailable",
                                       recovery_reason="provider-lost")
        with (plugin_run / "events.jsonl").open("w", encoding="utf-8") as out:
            for event in [proc] + events:
                out.write(json.dumps(event, sort_keys=True) + "\n")
        # The host's renders land outside the plug-in's run directory.
        shutil.move(plugin_run / "bounce.wav", self.run / "host-bounce.wav")
        _, channels = gfc.read_wav(plugin_run / "realtime.wav")
        channels = [list(ch) for ch in channels]
        channels[0][0] += 1e-7  # a real realtime render is not the bounce's bytes
        gfc.write_wav_f32(self.run / "host-realtime.wav", channels)
        (plugin_run / "realtime.wav").unlink()

    def test_a_simulated_logic_run_verifies_as_pass(self):
        self.assertTrue(self.preflight()[0])
        st = rhs.stage(self.run, fault="force_loss_before", at_sample=FAULT, window_post=POST,
                       stimulus_frames=FRAMES)
        rhs.snapshot_prefs(self.run, "logic")
        launched: list[list[str]] = []

        def execute(cmd):
            launched.append(cmd)
            self.procs.append((5151, LOGIC))

        st = rhs.launch(self.run, processes=lambda: list(self.procs), execute=execute,
                        timeout=1)
        self.assertEqual(launched[0][:3], ["open", "-g", "-n"])
        # The host was launched a while before its realtime render.
        st = rhs.save_state(self.run, launch_unix_us=st["launch_unix_us"] - 30_000_000)
        self.simulate_plugin(st, 5151)
        rhs.await_claim(self.run, timeout=1, poll=0.01)
        rhs.realtime_begin(self.run)
        realtime = self.run / "host-realtime.wav"
        st = rhs.save_state(self.run, realtime_started_unix_us=int(
            realtime.stat().st_mtime * 1e6) - 2_000_000)
        rhs.realtime_end(self.run, realtime, settle_s=0.05, poll=0.01)
        evidence = self.run / "ax.json"
        executable = self.run / "plugin-binary"
        executable.write_bytes(b"binary")
        # A snapshot showing the track armed on a real input is refused before
        # any receipt is written.
        evidence.write_text(json.dumps({"plugins": [rhs.PLUGIN_NAME], "insert_slot": 1,
                                        "track": dict(UNARMED, record_armed=True,
                                                      input="Input 1")}), encoding="utf-8")
        with self.assertRaisesRegex(SystemExit, "record-armed"):
            rhs.collect(self.run, bounce=self.run / "host-bounce.wav", host_evidence=evidence,
                        hosting_mode="in_process", host_alive=True, host_version="12.4",
                        code_commit="c" * 40, plugin_executable=executable, window_pre=1000)
        self.assertFalse((self.run / "artifacts").exists())
        evidence.write_text(json.dumps({"plugins": [rhs.PLUGIN_NAME], "insert_slot": 1,
                                        "track": UNARMED}), encoding="utf-8")
        receipt, verdict, outcome = rhs.collect(
            self.run, bounce=self.run / "host-bounce.wav", host_evidence=evidence,
            hosting_mode="in_process", host_alive=True, host_version="12.4",
            code_commit="c" * 40, plugin_executable=executable, window_pre=1000)
        self.assertEqual((verdict, outcome), ("pass", "pass"), receipt["verdict_reasons"])
        self.assertEqual(receipt["binding"]["outcome"], "bound")
        self.assertEqual(receipt["host"]["launch"].split()[:3], ["open", "-g", "-n"])

    def test_a_realtime_render_faster_than_realtime_is_not_evidence(self):
        self.assertTrue(self.preflight()[0])
        rhs.stage(self.run, fault="force_loss_before", at_sample=FAULT, window_post=POST,
                  stimulus_frames=FRAMES)
        artifact = self.run / "fast.wav"
        rhs.realtime_begin(self.run)
        gfc.write_wav_f32(artifact, [[0.0] * 10] * 2)
        st = rhs.realtime_end(self.run, artifact, settle_s=0.05, poll=0.01)
        record = st["realtime_provenance"]
        # The record is honest: begun and finished within a fraction of the
        # 1.2 s of audio, which v2's provenance check then rejects.
        self.assertLess(record["finished_unix_us"] - record["started_unix_us"], 1_000_000)
        (self.run / hosts.REALTIME_PROVENANCE).write_text(json.dumps(record), encoding="utf-8")
        problem = hosts.realtime_provenance_problem(
            self.run, "realtime.wav", {"id": "logic"}, FRAMES + POST, None)
        self.assertIn("faster than realtime", problem)


if __name__ == "__main__":
    unittest.main()
