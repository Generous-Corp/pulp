#!/usr/bin/env python3
"""Destructive-path checks for Forge's generated-module writer."""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import generate  # noqa: E402
import patch  # noqa: E402
import rack_open  # noqa: E402


class RackPluginDiscoverySafety(unittest.TestCase):
    @staticmethod
    def write_plugin(directory: pathlib.Path, slug: str = "MakerPack") -> None:
        package = directory / slug
        package.mkdir(parents=True)
        (package / "plugin.json").write_text(json.dumps({
            "slug": slug,
            "name": "Maker Pack",
            "brand": "Named Maker",
            "version": "2.0.0",
            "modules": [{"slug": "Voice", "name": "Voice"}],
        }))

    def test_plugin_directory_created_after_import_is_discovered(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            home = pathlib.Path(tmp) / "home"
            home.mkdir()
            with mock.patch.dict(os.environ, {"HOME": str(home)}, clear=False):
                os.environ.pop("RACK_PLUGIN_DIR", None)
                self.assertEqual([], patch.rack_plugin_dirs())

                plugin_dir = (home / "Library/Application Support/Rack2" /
                              f"plugins-{patch.rack_arch()}")
                self.write_plugin(plugin_dir)

                self.assertEqual([str(plugin_dir)], patch.rack_plugin_dirs())
                with mock.patch.object(
                        patch, "_repair_bundled_pack", return_value=False):
                    inventory = patch.inventory()
                self.assertIn("Voice", inventory["MakerPack"]["modules"])

    def test_explicit_plugin_directory_is_exclusive_and_installable(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            home_plugins = (root / "home/Library/Application Support/Rack2" /
                            f"plugins-{patch.rack_arch()}")
            override = root / "host-visible-plugins"
            self.write_plugin(home_plugins, "IgnoredPack")
            self.write_plugin(override, "MakerPack")
            with mock.patch.dict(os.environ, {
                    "HOME": str(root / "home"),
                    "RACK_PLUGIN_DIR": str(override),
            }, clear=False):
                self.assertEqual(str(override), patch.rack_plugin_dir())
                self.assertEqual([str(override)], patch.rack_plugin_dirs())
                self.assertEqual(str(override), generate.plugin_dir())
                with mock.patch.object(
                        patch, "_repair_bundled_pack", return_value=False):
                    inventory = patch.inventory()
            self.assertIn("MakerPack", inventory)
            self.assertNotIn("IgnoredPack", inventory)

    def test_bundled_pack_repair_targets_explicit_plugin_directory(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            home = root / "home"
            app = root / "Forge Modular.app"
            rack = app / "Contents/Resources/rack"
            rack.mkdir(parents=True)
            (rack / "ForgeModular-2.0.0-mac-arm64.vcvplugin").write_bytes(
                b"pack bytes")
            placer = app / "Contents/Resources/install_pack.sh"
            source_placer = (pathlib.Path(__file__).parents[2] /
                             "examples/forge-modular/install_pack.sh")
            shutil.copy2(source_placer, placer)
            placer.chmod(0o755)
            override = root / "host-visible-plugins"
            with mock.patch.dict(os.environ, {
                    "HOME": str(home),
                    "FORGE_MODULAR_APP": str(app),
                    "RACK_PLUGIN_DIR": str(override),
            }, clear=False):
                self.assertTrue(patch._repair_bundled_pack())
                self.assertTrue((override /
                    "ForgeModular-2.0.0-mac-arm64.vcvplugin").is_file())
                default = (home / "Library/Application Support/Rack2" /
                           f"plugins-{patch.rack_arch()}")
                self.assertFalse(default.exists())

    def test_explicit_destination_backs_up_within_its_grant(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            app = root / "Forge Modular.app"
            rack = app / "Contents/Resources/rack"
            rack.mkdir(parents=True)
            (rack / "ForgeModular-2.0.0-mac-arm64.vcvplugin").write_bytes(
                b"pack bytes")
            placer = pathlib.Path(__file__).parents[2] / \
                "examples/forge-modular/install_pack.sh"
            override = root / "host-visible-plugins"
            unpacked = override / "ForgeModular"
            unpacked.mkdir(parents=True)
            (unpacked / "plugin.json").write_text(
                '{"slug":"ForgeModular","version":"1.0.0"}\n')

            result = __import__("subprocess").run([
                str(placer), "--source", str(app), "--home", str(root / "home"),
                "--dest", str(override),
            ], capture_output=True, text=True)

            self.assertEqual(0, result.returncode, result.stderr)
            backups = override / ".Forge-Modular-replaced-packs"
            self.assertEqual(1, len(list(backups.glob("ForgeModular-*"))))
            self.assertFalse((root / ".Forge-Modular-replaced-packs").exists())


class ExclusiveMakerSafety(unittest.TestCase):
    mentions = {
        "Named Maker": {
            "slugs": ["MakerPack", "MakerUtilities"],
            "exhaustive": False,
            "exclusive": True,
        },
    }

    def test_named_maker_modules_and_core_are_the_complete_allowlist(self) -> None:
        candidate = {"modules": [
            {"id": 1, "plugin": "MakerPack", "model": "Voice"},
            {"id": 2, "plugin": "MakerUtilities", "model": "Mixer"},
            {"id": 3, "plugin": "Core", "model": "AudioInterface2"},
        ]}
        self.assertEqual(
            [], patch.exclusive_maker_errors(candidate, self.mentions))

    def test_prefix_exclusive_multi_maker_list_forms_one_allowlist(self) -> None:
        catalog = {
            "Bogaudio": {"brand": "Bogaudio", "arches": ["mac-arm64"]},
            "Valley": {"brand": "Valley", "arches": ["mac-arm64"]},
        }
        mentions = patch.brand_mentions("use only Bogaudio and Valley", catalog)
        self.assertEqual(
            {"Bogaudio", "Valley"}, patch.exclusive_maker_plugins(mentions))

    def test_substitution_and_zero_named_modules_are_both_rejected(self) -> None:
        candidate = {"modules": [
            {"id": 1, "plugin": "LookalikePack", "model": "Voice"},
            {"id": 2, "plugin": "Core", "model": "AudioInterface2"},
        ]}
        errors = patch.exclusive_maker_errors(candidate, self.mentions)
        self.assertTrue(any("LookalikePack" in error for error in errors))
        self.assertTrue(any("at least one module" in error for error in errors))

    def test_malformed_model_shapes_are_retryable_errors(self) -> None:
        self.assertTrue(patch.exclusive_maker_errors([], self.mentions))
        self.assertTrue(patch.exclusive_maker_errors(
            {"modules": "bad"}, self.mentions))
        self.assertTrue(patch.exclusive_maker_errors(
            {"modules": ["bad"]}, self.mentions))
        self.assertTrue(patch.exclusive_maker_errors(
            {"modules": [{"plugin": []}]}, self.mentions))

    def test_nonexclusive_mention_does_not_restrict_the_patch(self) -> None:
        mentions = {
            "Named Maker": {
                "slugs": ["MakerPack"],
                "exhaustive": False,
                "exclusive": False,
            },
        }
        candidate = {"modules": [
            {"id": 1, "plugin": "LookalikePack", "model": "Voice"},
        ]}
        self.assertEqual([], patch.exclusive_maker_errors(candidate, mentions))

    def test_missing_named_maker_inventory_stops_before_generation(self) -> None:
        empty = {"MakerPack": {"modules": {}}}
        self.assertTrue(patch.exclusive_maker_inventory_errors(
            empty, self.mentions))
        available = {
            "MakerPack": {"modules": {"Voice": {}}},
            "Core": {"modules": {"AudioInterface2": {}}},
        }
        self.assertEqual([], patch.exclusive_maker_inventory_errors(
            available, self.mentions))


class ToolchainHeaderClosureSafety(unittest.TestCase):
    def test_curated_dsp_transitive_headers_must_ship_before_provider_use(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            signal = root / "core/signal/include/pulp/signal"
            signal.mkdir(parents=True)
            (signal / "primitive.hpp").write_text(
                "#include <pulp/timebase/beat_division.hpp>\n")
            registry = root / "tools/rack/knowledge/module/dsp-primitives.json"
            registry.parent.mkdir(parents=True)
            registry.write_text(json.dumps({
                "capabilities": [{"include": "primitive.hpp"}],
            }))

            with mock.patch.object(generate, "INCLUDES", ["core/signal"]):
                self.assertEqual(
                    [f"pulp/timebase/beat_division.hpp (required by "
                     f"{signal / 'primitive.hpp'})"],
                    generate.missing_public_header_dependencies(str(root)))

            timebase = root / "core/timebase/include/pulp/timebase"
            timebase.mkdir(parents=True)
            (timebase / "beat_division.hpp").write_text("#pragma once\n")
            with mock.patch.object(
                    generate, "INCLUDES", ["core/signal", "core/timebase"]):
                self.assertEqual(
                    [], generate.missing_public_header_dependencies(str(root)))


class RackLaunchSafety(unittest.TestCase):
    def launch_fixture(self, artifact_name="demo.vcv", module_count=1):
        temp = tempfile.TemporaryDirectory()
        root = pathlib.Path(temp.name)
        app = root / "VCV Rack 2 Free.app"
        app.mkdir()
        artifact = root / artifact_name
        artifact.write_text(json.dumps({
            "modules": [{"id": index, "plugin": "Core",
                         "model": "AudioInterface2"}
                        for index in range(1, module_count + 1)],
            "cables": []}))
        log = root / "log.txt"
        log.write_text("")
        return temp, str(app), str(artifact), str(log)

    def test_macos_launch_delivers_patch_as_a_document(self) -> None:
        artifact = "/tmp/a generated patch.vcv"
        app = "/Applications/VCV Rack 2 Free.app"
        self.assertEqual(
            [sys.executable, str(HERE / "rack_open.py"), "--app",
             app, "--patch", artifact],
            generate.rack_launch_command(artifact, platform="darwin", app=app))
        self.assertEqual(
            [generate.RACK_APP, artifact],
            generate.rack_launch_command(artifact, platform="linux"))
        self.assertEqual(
            "open it with:  " + pathlib.Path(sys.executable).as_posix() + " " +
            str(HERE / "rack_open.py") + " --app "
            "'/Applications/VCV Rack 2 Free.app' --patch "
            "'/tmp/a generated patch.vcv'",
            generate.rack_open_instruction(
                artifact, platform="darwin", app=app))

    def test_patch_count_and_hash_come_from_one_byte_snapshot(self) -> None:
        payload = json.dumps({
            "modules": [{"id": 1}, {"id": 2}], "cables": []}).encode()
        with mock.patch.object(pathlib.Path, "read_bytes",
                               return_value=payload) as read:
            modules, digest = rack_open._patch_identity("/tmp/changing.vcv")
        read.assert_called_once_with()
        self.assertEqual(2, modules)
        self.assertEqual(hashlib.sha256(payload).hexdigest(), digest)

    def test_rack_edition_selection_prefers_the_installed_pro_product(self) -> None:
        pro, free, generic = generate.RACK_APPS
        self.assertEqual(
            pro, generate.select_rack_app([pro, free], [pro]))
        self.assertEqual(
            pro, generate.select_rack_app([pro, free], [free]))
        self.assertEqual(
            pro, generate.select_rack_app([pro, free], []))
        self.assertEqual(
            pro, generate.select_rack_app([pro, generic, free], [pro, free]))
        self.assertEqual(
            pro, generate.select_rack_app([pro, generic, free], [generic]))
        self.assertEqual(
            free, generate.select_rack_app([generic, free], []))
        self.assertIsNone(generate.select_rack_app([], []))

        # Exhaust the small state space. A stale sibling process cannot override
        # the installed preference: Pro, Free, then the legacy generic bundle.
        for installed_bits in range(8):
            installed = [app for i, app in enumerate(generate.RACK_APPS)
                         if installed_bits & (1 << i)]
            for running_bits in range(8):
                running = [app for i, app in enumerate(generate.RACK_APPS)
                           if running_bits & (1 << i) and app in installed]
                expected = next((app for app in generate.RACK_APPS
                                 if app in installed), None)
                self.assertEqual(
                    expected, generate.select_rack_app(installed, running))

    def test_macos_rack_detector_error_never_launches_an_edition(self) -> None:
        pro = generate.RACK_APPS[0]
        with mock.patch.object(generate.sys, "platform", "darwin"), \
                mock.patch.object(generate.os.path, "isdir",
                               side_effect=lambda path: path == pro), \
                mock.patch.object(rack_open, "rack_running",
                                  side_effect=RuntimeError("pgrep exit 2")), \
                mock.patch.object(generate.subprocess, "Popen") as popen:
            with self.assertRaisesRegex(RuntimeError, "pgrep exit 2"):
                generate.launch("/tmp/demo.vcv")
        popen.assert_not_called()

    def test_module_and_patch_generators_share_the_edition_policy(self) -> None:
        app_names = tuple(pathlib.Path(app).name.removesuffix(".app")
                          for app in generate.RACK_APPS)
        self.assertEqual(app_names, patch.RACK_APPS)
        pro, free, generic = patch.RACK_APPS
        self.assertEqual(
            pro, patch.rack_app_name(
                running=lambda name: False,
                installed=lambda name: name in {pro, free}))
        self.assertEqual(
            pro, patch.rack_app_name(
                running=lambda name: name == free,
                installed=lambda name: name in {pro, free}))
        self.assertEqual(
            pro, patch.rack_app_name(
                running=lambda name: name == generic,
                installed=lambda name: name in {pro, free, generic}))
        self.assertEqual(
            free, patch.rack_app_name(
                running=lambda name: False,
                installed=lambda name: name in {free, generic}))
        self.assertEqual(
            generic, patch.rack_app_name(
                running=lambda name: False,
                installed=lambda name: name == generic))

    def test_module_restart_never_treats_detector_error_as_stopped(self) -> None:
        name = "VCV Rack 2 Pro"
        app = "/Applications/VCV Rack 2 Pro.app"
        with mock.patch.object(patch, "rack_app_name", return_value=name), \
                mock.patch.object(rack_open, "rack_running",
                                  side_effect=[True,
                                               RuntimeError("pgrep exit 2")]), \
                mock.patch("subprocess.run",
                           return_value=mock.Mock(returncode=0)) as run:
            with self.assertRaisesRegex(RuntimeError, "pgrep exit 2"):
                patch.restart_rack()
        self.assertEqual(
            [mock.call(["osascript", "-e",
                        'tell application "VCV Rack 2 Pro" to quit'],
                       capture_output=True)],
            run.call_args_list)
        self.assertNotIn(mock.call(["open", "-a", name], capture_output=True),
                         run.call_args_list)

        # A different running edition is not confused with the selected app.
        with mock.patch.object(patch, "rack_app_name", return_value=name), \
                mock.patch.object(rack_open, "rack_running",
                                  return_value=False) as running, \
                mock.patch("subprocess.run") as run:
            self.assertEqual(
                (True, "Rack was not running; it will pick the module up next launch"),
                patch.restart_rack())
        running.assert_called_once_with(app)
        run.assert_not_called()

    def test_rack_evidence_is_exact_ordered_and_complete(self) -> None:
        expected = "/tmp/demo.vcv"
        self.assertTrue(rack_open.open_evidence(
            "Loading patch /tmp/demo.vcv\nCreating module Forge A\n"
            "Creating module Core Audio\n", expected, 2))
        self.assertFalse(rack_open.open_evidence(
            "Loading patch /tmp/demo.vcv.old\nCreating module Forge A\n",
            expected, 1))
        self.assertFalse(rack_open.open_evidence(
            "Creating module Forge AUTOSAVE\nLoading patch /tmp/demo.vcv\n",
            expected, 1))
        self.assertFalse(rack_open.open_evidence(
            "Loading patch /tmp/demo.vcv\nCreating module Forge A\n",
            expected, 2))
        self.assertFalse(rack_open.open_evidence(
            "Loading patch /tmp/demo.vcv\nCreating module Forge A\n"
            "Loading patch /tmp/other.vcv\nCreating module Forge B\n",
            expected, 2))

    def test_latest_evidence_requires_latest_exact_patch_and_module_count(self) -> None:
        expected = "/tmp/a generated patch.vcv"
        earlier_only = (
            f'Loading patch "{expected}"\n'
            "Creating module Forge A\nCreating module Forge B\n"
            "Loading patch /tmp/other.vcv\nCreating module Forge C\n")
        self.assertFalse(
            rack_open.latest_open_evidence(earlier_only, expected, 2))

        latest_exact = (
            "Loading patch /tmp/other.vcv\nCreating module Forge C\n"
            f'Loading patch "{expected}"\n'
            "Creating module widget Forge A\n"
            "Creating module Forge A\nCreating module Forge B\n")
        self.assertEqual(
            [f'Loading patch "{expected}"',
             "Creating module Forge A", "Creating module Forge B"],
            rack_open.latest_open_evidence(latest_exact, expected, 2))
        self.assertFalse(
            rack_open.latest_open_evidence(latest_exact, expected, 3))

    def test_running_rack_detection_uses_macos_supported_regex(self) -> None:
        app = "/Applications/VCV Rack 2 Free.app"
        with mock.patch.object(rack_open.subprocess, "run",
                               return_value=mock.Mock(returncode=0)) as run:
            self.assertTrue(rack_open.rack_running(app))
        self.assertEqual(
            ["pgrep", "-f",
             r"^/Applications/VCV\ Rack\ 2\ Free\.app/Contents/MacOS/Rack($| )"],
            run.call_args.args[0])

        with mock.patch.object(rack_open.subprocess, "run",
                               return_value=mock.Mock(returncode=1)):
            self.assertFalse(rack_open.rack_running(app))
        with mock.patch.object(rack_open.subprocess, "run",
                               return_value=mock.Mock(returncode=2)):
            with self.assertRaisesRegex(RuntimeError, "pgrep exit 2"):
                rack_open.rack_running(app)

    def test_rack_process_identity_binds_pid_and_start_time(self) -> None:
        app = "/Applications/VCV Rack 2 Pro.app"
        process = "72076:1786570779.435535"
        with mock.patch.object(
                rack_open.subprocess, "run",
                return_value=mock.Mock(returncode=0, stdout="72076\n")) as run, \
                mock.patch.object(
                    rack_open, "_native_process_identity",
                    return_value=process) as native_identity:
            self.assertEqual(
                process, rack_open._rack_process_identity(app))
        run.assert_called_once_with(
            ["pgrep", "-f",
             r"^/Applications/VCV\ Rack\ 2\ Pro\.app/Contents/MacOS/Rack($| )"],
            capture_output=True, text=True, timeout=2.0)
        native_identity.assert_called_once_with(
            72076, rack_open._app_binary(app))

        with mock.patch.object(
                rack_open.subprocess, "run",
                return_value=mock.Mock(returncode=0, stdout="41\n42\n")), \
                mock.patch.object(
                    rack_open, "_native_process_identity") as native_identity:
            self.assertIsNone(rack_open._rack_process_identity(app))
        native_identity.assert_not_called()

    def test_native_process_identity_revalidates_binary_and_start_time(self) -> None:
        pid = 72076
        binary = "/Applications/VCV Rack 2 Pro.app/Contents/MacOS/Rack"
        library = mock.Mock()

        def read_path(actual_pid, buffer, _buffer_size):
            self.assertEqual(pid, actual_pid)
            buffer.value = os.fsencode(binary)
            return len(buffer.value)

        def read_info(actual_pid, flavor, argument, buffer, buffer_size):
            self.assertEqual(pid, actual_pid)
            self.assertEqual(rack_open._PROC_PIDTBSDINFO, flavor)
            self.assertEqual(0, argument)
            self.assertEqual(
                rack_open.ctypes.sizeof(rack_open._ProcBsdInfo), buffer_size)
            info = rack_open.ctypes.cast(
                buffer,
                rack_open.ctypes.POINTER(rack_open._ProcBsdInfo)).contents
            info.pbi_pid = pid
            info.pbi_start_tvsec = 1786570779
            info.pbi_start_tvusec = 435535
            return buffer_size

        library.proc_pidpath.side_effect = read_path
        library.proc_pidinfo.side_effect = read_info
        self.assertEqual(
            "72076:1786570779.435535",
            rack_open._native_process_identity(
                pid, binary, _library=library))
        library.proc_pidpath.assert_called_once()
        library.proc_pidinfo.assert_called_once()

    def test_native_process_identity_rejects_mismatch_or_denied_info(self) -> None:
        pid = 72076
        binary = "/Applications/VCV Rack 2 Pro.app/Contents/MacOS/Rack"

        wrong_binary = mock.Mock()

        def read_wrong_path(_pid, buffer, _buffer_size):
            buffer.value = b"/Applications/VCV Rack 2 Free.app/Contents/MacOS/Rack"
            return len(buffer.value)

        wrong_binary.proc_pidpath.side_effect = read_wrong_path
        self.assertIsNone(rack_open._native_process_identity(
            pid, binary, _library=wrong_binary))
        wrong_binary.proc_pidinfo.assert_not_called()

        denied = mock.Mock()

        def read_expected_path(_pid, buffer, _buffer_size):
            buffer.value = os.fsencode(binary)
            return len(buffer.value)

        denied.proc_pidpath.side_effect = read_expected_path
        denied.proc_pidinfo.return_value = -1
        self.assertIsNone(rack_open._native_process_identity(
            pid, binary, _library=denied))

        with mock.patch.object(rack_open, "_load_libproc", return_value=None):
            self.assertIsNone(rack_open._native_process_identity(pid, binary))

    def test_focus_activates_the_exact_app_without_a_document(self) -> None:
        app = "/Applications/VCV Rack 2 Pro.app"
        with mock.patch.object(rack_open.subprocess, "run") as run:
            rack_open._focus(app)
        run.assert_called_once_with(
            ["/usr/bin/open", "-a", app], check=True)

    def test_cold_and_warm_success_never_quit(self) -> None:
        for already_running in (False, True):
            with self.subTest(already_running=already_running):
                temp, app, artifact, log = self.launch_fixture()
                with temp, \
                        mock.patch.object(rack_open, "rack_running",
                                          return_value=already_running), \
                        mock.patch.object(rack_open, "_document_open") as opened, \
                        mock.patch.object(rack_open, "_wait_for_evidence",
                                          return_value=["proved"]), \
                        mock.patch.object(rack_open, "_quit") as quit_app:
                    self.assertEqual(
                        ["proved"], rack_open.open_patch(app, artifact, log))
                opened.assert_called_once_with(app, os.path.realpath(artifact))
                quit_app.assert_not_called()

    def test_running_exact_patch_focuses_without_handoff_or_quit(self) -> None:
        temp, app, artifact, log = self.launch_fixture(
            artifact_name="a generated patch.vcv", module_count=2)
        pathlib.Path(log).write_text(
            f'Loading patch "{os.path.realpath(artifact)}"\n'
            "Creating module Forge A\nCreating module Core Audio\n")
        with mock.patch.object(rack_open, "_rack_process_identity",
                               return_value="72076:Sat Aug  8 18:00:00 2026"):
            rack_open._record_verified_open(
                app, artifact, 2, log, rack_open._file_sha256(artifact))
        with temp, \
                mock.patch.object(rack_open, "rack_running", return_value=True), \
                mock.patch.object(rack_open, "_rack_process_identity",
                                  return_value="72076:Sat Aug  8 18:00:00 2026"), \
                mock.patch.object(rack_open, "_focus") as focus, \
                mock.patch.object(rack_open, "_document_open") as opened, \
                mock.patch.object(rack_open, "_wait_for_evidence") as waited, \
                mock.patch.object(rack_open, "_quit") as quit_app, \
                mock.patch.object(rack_open, "_wait_stopped") as stopped:
            evidence = rack_open.open_patch(app, artifact, log)
        self.assertEqual(
            [f'Loading patch "{os.path.realpath(artifact)}"',
             "Creating module Forge A", "Creating module Core Audio"],
            evidence)
        focus.assert_called_once_with(app)
        opened.assert_not_called()
        waited.assert_not_called()
        quit_app.assert_not_called()
        stopped.assert_not_called()

    def test_unrelated_trailing_log_growth_preserves_same_patch_focus(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        process = "72076:Sat Aug  8 18:00:00 2026"
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge A\n")
        with mock.patch.object(rack_open, "_rack_process_identity",
                               return_value=process):
            rack_open._record_verified_open(
                app, artifact, 1, log, rack_open._file_sha256(artifact))
        with pathlib.Path(log).open("a") as stream:
            stream.write(
                "Creating module widget Forge A\n"
                "Loaded SVG res/panel.svg\n"
                "Network request completed\n"
                "Autosaving patch\nSaving settings\n")
        with temp, \
                mock.patch.object(rack_open, "rack_running", return_value=True), \
                mock.patch.object(rack_open, "_rack_process_identity",
                                  return_value=process), \
                mock.patch.object(rack_open, "_focus") as focus, \
                mock.patch.object(rack_open, "_document_open") as opened, \
                mock.patch.object(rack_open, "_quit") as quit_app:
            evidence = rack_open.open_patch(app, artifact, log)
        self.assertEqual(
            [f"Loading patch {os.path.realpath(artifact)}",
             "Creating module Forge A"], evidence)
        focus.assert_called_once_with(app)
        opened.assert_not_called()
        quit_app.assert_not_called()

    def test_identity_settle_is_bounded_when_segment_never_stabilizes(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        process = "72076:Sat Aug  8 18:00:00 2026"
        selected = rack_open._file_sha256(artifact)
        logs = [
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge A\n",
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge B\n",
        ]
        reads = 0
        now = 0.0

        def changing_log(_path):
            nonlocal reads
            value = logs[reads % 2]
            reads += 1
            return value

        def clock():
            return now

        def sleep(seconds):
            nonlocal now
            now += seconds

        with temp, \
                mock.patch.object(rack_open, "_read",
                                  side_effect=changing_log), \
                mock.patch.object(rack_open, "_rack_process_identity",
                                  return_value=process):
            rack_open._record_verified_open(
                app, artifact, 1, log, selected,
                settle_timeout=0.2, poll_interval=0.05,
                _clock=clock, _sleep=sleep)
            self.assertFalse(rack_open._identity_path(log).exists())
        self.assertGreaterEqual(reads, 2)
        self.assertLessEqual(now, 0.2)

    def test_record_never_rebinds_to_a_later_completed_load(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        process = "72076:Sat Aug  8 18:00:00 2026"
        selected = rack_open._file_sha256(artifact)
        proven = [
            f"Loading patch {os.path.realpath(artifact)}",
            "Creating module Forge Original",
        ]
        # A second same-path load completed before recording began. Path,
        # process, module count and selected bytes all still agree; only the
        # invocation-owned evidence prevents rebinding to this later load.
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge Replacement\n")
        with temp, mock.patch.object(rack_open, "_rack_process_identity",
                                     return_value=process):
            rack_open._record_verified_open(
                app, artifact, 1, log, selected,
                proven_evidence=proven)
            self.assertFalse(rack_open._identity_path(log).exists())

    def test_invalid_second_snapshot_aborts_without_rebinding(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        selected = rack_open._file_sha256(artifact)
        identity = {
            "version": 3, "app": os.path.realpath(app),
            "patch": os.path.realpath(artifact),
            "patch_sha256": selected, "modules": 1,
            "log_inode": os.stat(log).st_ino, "load_count": 1,
            "evidence_sha256": "proof", "process_identity": "41:start",
        }
        evidence = ["Loading patch exact", "Creating module Forge A"]
        with temp, \
                mock.patch.object(
                    rack_open, "_open_identity_snapshot",
                    side_effect=[(identity, evidence), None]) as sampled, \
                mock.patch.object(rack_open, "_write_identity") as written:
            rack_open._record_verified_open(
                app, artifact, 1, log, selected,
                settle_timeout=0.2, poll_interval=0.0)
        self.assertEqual(2, sampled.call_count)
        written.assert_not_called()

    def test_patch_change_during_recording_fails_exact_open_verification(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        selected = rack_open._file_sha256(artifact)
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge A\n")
        pathlib.Path(artifact).write_text(json.dumps({
            "modules": [{"id": 1, "plugin": "Fundamental", "model": "VCO"}],
            "cables": []}))
        with temp, self.assertRaisesRegex(RuntimeError,
                                          "selected Rack patch changed"):
            rack_open._record_verified_open(
                app, artifact, 1, log, selected,
                proven_evidence=[
                    f"Loading patch {os.path.realpath(artifact)}",
                    "Creating module Forge A",
                ])

    def test_unreadable_patch_during_recording_fails_exact_open_verification(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        selected = rack_open._file_sha256(artifact)
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge A\n")
        pathlib.Path(artifact).unlink()
        with temp, self.assertRaisesRegex(RuntimeError,
                                          "selected Rack patch became unreadable"):
            rack_open._record_verified_open(
                app, artifact, 1, log, selected,
                proven_evidence=[
                    f"Loading patch {os.path.realpath(artifact)}",
                    "Creating module Forge A",
                ])

    def test_snapshot_finishing_at_deadline_cannot_publish_identity(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        selected = rack_open._file_sha256(artifact)
        identity = {
            "version": 3, "app": os.path.realpath(app),
            "patch": os.path.realpath(artifact),
            "patch_sha256": selected, "modules": 1,
            "log_inode": os.stat(log).st_ino, "load_count": 1,
            "evidence_sha256": "proof", "process_identity": "41:start",
        }
        evidence = ["Loading patch exact", "Creating module Forge A"]
        now = 0.0

        def clock():
            return now

        def slow_snapshot(*_args, **_kwargs):
            nonlocal now
            now += 0.06
            return identity, evidence

        with temp, \
                mock.patch.object(rack_open, "_open_identity_snapshot",
                                  side_effect=slow_snapshot), \
                mock.patch.object(rack_open, "_write_identity") as written:
            rack_open._record_verified_open(
                app, artifact, 1, log, selected,
                settle_timeout=0.1, poll_interval=0.0,
                _clock=clock, _sleep=lambda _seconds: None)
        self.assertGreaterEqual(now, 0.1)
        written.assert_not_called()

    def test_same_path_rewrite_with_same_module_count_is_not_existing_identity(self) -> None:
        temp, app, artifact, log = self.launch_fixture(
            artifact_name="refined patch.vcv")
        pathlib.Path(log).write_text(
            f'Loading patch "{os.path.realpath(artifact)}"\n'
            "Creating module Forge Before\n")
        with mock.patch.object(rack_open, "_rack_process_identity",
                               return_value="72076:Sat Aug  8 18:00:00 2026"):
            rack_open._record_verified_open(
                app, artifact, 1, log, rack_open._file_sha256(artifact))
        # Normal refinement overwrites patch.vcv in place. Keeping one module
        # proves path + module count is not content identity.
        pathlib.Path(artifact).write_text(json.dumps({
            "modules": [{"id": 1, "plugin": "Fundamental", "model": "VCO"}],
            "cables": []}))
        with temp, \
                mock.patch.object(rack_open, "rack_running", return_value=True), \
                mock.patch.object(rack_open, "_focus") as focus, \
                mock.patch.object(rack_open, "_document_open") as opened, \
                mock.patch.object(rack_open, "_wait_for_evidence",
                                  return_value=["proved fresh"]), \
                mock.patch.object(rack_open, "_quit") as quit_app:
            self.assertEqual(
                ["proved fresh"], rack_open.open_patch(app, artifact, log))
        focus.assert_not_called()
        opened.assert_called_once_with(app, os.path.realpath(artifact))
        quit_app.assert_not_called()

    def test_focus_is_bound_to_the_invocation_selected_digest(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        original = pathlib.Path(artifact).read_bytes()
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge Original\n")
        process = "41:Sat Aug  8 18:00:00 2026"
        with mock.patch.object(rack_open, "_rack_process_identity",
                               return_value=process):
            rack_open._record_verified_open(
                app, artifact, 1, log, rack_open._file_sha256(artifact))

        # Select replacement bytes, then simulate a concurrent rollback to the
        # cached bytes before the shortcut checks the path again.
        pathlib.Path(artifact).write_text(json.dumps({
            "modules": [{"id": 1, "plugin": "Fundamental", "model": "VCO"}],
            "cables": []}))
        _, selected_replacement = rack_open._patch_identity(artifact)
        pathlib.Path(artifact).write_bytes(original)
        with temp, mock.patch.object(rack_open, "_rack_process_identity",
                                     return_value=process):
            self.assertFalse(rack_open._verified_existing_evidence(
                app, artifact, 1, log, selected_replacement))

    def test_same_inode_log_truncation_cannot_reuse_prior_rack_identity(self) -> None:
        temp, app, artifact, log = self.launch_fixture(
            artifact_name="same path.vcv")
        original = pathlib.Path(artifact).read_bytes()
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge Original\n")
        original_inode = os.stat(log).st_ino
        with mock.patch.object(rack_open, "_rack_process_identity",
                               return_value="41:Sat Aug  8 18:00:00 2026"):
            rack_open._record_verified_open(
                app, artifact, 1, log, rack_open._file_sha256(artifact))

        # Rack truncates log.txt in place on restart. Simulate a new run that
        # loaded different bytes at the same path, then the file being restored
        # to the old bytes. Inode, load count, path, hash and module count now
        # all collide; only process start identity distinguishes the Rack run.
        pathlib.Path(artifact).write_text(json.dumps({
            "modules": [{"id": 1, "plugin": "Fundamental", "model": "VCO"}],
            "cables": []}))
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge Replacement\n")
        self.assertEqual(original_inode, os.stat(log).st_ino)
        pathlib.Path(artifact).write_bytes(original)

        with temp, \
                mock.patch.object(rack_open, "rack_running", return_value=True), \
                mock.patch.object(rack_open, "_rack_process_identity",
                                  return_value="42:Sat Aug  8 18:05:00 2026"), \
                mock.patch.object(rack_open, "_focus") as focus, \
                mock.patch.object(rack_open, "_document_open") as opened, \
                mock.patch.object(rack_open, "_wait_for_evidence",
                                  return_value=["proved fresh"]), \
                mock.patch.object(rack_open, "_quit") as quit_app:
            self.assertEqual(
                ["proved fresh"], rack_open.open_patch(app, artifact, log))
        focus.assert_not_called()
        opened.assert_called_once_with(app, os.path.realpath(artifact))
        quit_app.assert_not_called()

    def test_same_process_log_replacement_cannot_reuse_verified_record(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge Original\n")
        process = "41:Sat Aug  8 18:00:00 2026"
        with mock.patch.object(rack_open, "_rack_process_identity",
                               return_value=process):
            rack_open._record_verified_open(
                app, artifact, 1, log, rack_open._file_sha256(artifact))
        # Same inode, process, path and load count, but this text did not
        # produce the verified record and must not inherit it.
        original_inode = os.stat(log).st_ino
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge Replacement\n")
        self.assertEqual(original_inode, os.stat(log).st_ino)
        with temp, mock.patch.object(rack_open, "_rack_process_identity",
                                     return_value=process):
            self.assertFalse(rack_open._matches_recorded_identity(
                app, artifact, 1, log, rack_open._file_sha256(artifact)))

    def test_unwritable_identity_cache_does_not_fail_verified_open(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge A\n")
        with temp, \
                mock.patch.object(rack_open, "_rack_process_identity",
                                  return_value="41:Sat Aug  8 18:00:00 2026"), \
                mock.patch.object(rack_open.tempfile, "mkstemp",
                                  side_effect=PermissionError("read only")):
            # Persistence is only a prerequisite for a later focus shortcut;
            # it cannot invalidate the exact load that already succeeded.
            rack_open._record_verified_open(
                app, artifact, 1, log, rack_open._file_sha256(artifact))

    def test_process_change_while_recording_does_not_publish_identity(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge A\n")
        with temp, mock.patch.object(
                rack_open, "_rack_process_identity",
                side_effect=["41:Sat Aug  8 18:00:00 2026",
                             "42:Sat Aug  8 18:00:01 2026"]):
            rack_open._record_verified_open(
                app, artifact, 1, log, rack_open._file_sha256(artifact))
            self.assertFalse(rack_open._identity_path(log).exists())

    def test_unavailable_process_identity_does_not_publish_identity_cache(
            self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge A\n")
        with temp, mock.patch.object(
                rack_open, "_rack_process_identity", return_value=None):
            rack_open._record_verified_open(
                app, artifact, 1, log, rack_open._file_sha256(artifact))
            self.assertFalse(rack_open._identity_path(log).exists())

    def test_missing_process_identity_cannot_enable_focus_shortcut(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge A\n")
        with mock.patch.object(rack_open, "_rack_process_identity",
                               return_value="41:Sat Aug  8 18:00:00 2026"):
            rack_open._record_verified_open(
                app, artifact, 1, log, rack_open._file_sha256(artifact))
        identity_path = rack_open._identity_path(log)
        identity = json.loads(identity_path.read_text())
        identity["process_identity"] = None
        identity_path.write_text(json.dumps(identity))
        with temp, mock.patch.object(rack_open, "_rack_process_identity",
                                     return_value=None):
            self.assertFalse(rack_open._matches_recorded_identity(
                app, artifact, 1, log, rack_open._file_sha256(artifact)))

    def test_log_change_during_identity_check_abandons_focus(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge A\n")
        process = "41:Sat Aug  8 18:00:00 2026"
        with mock.patch.object(rack_open, "_rack_process_identity",
                               return_value=process):
            rack_open._record_verified_open(
                app, artifact, 1, log, rack_open._file_sha256(artifact))
        snapshot = pathlib.Path(log).read_text()
        with temp, \
                mock.patch.object(rack_open, "_rack_process_identity",
                                  return_value=process), \
                mock.patch.object(rack_open, "_read",
                                  return_value=snapshot +
                                  "Loading patch /tmp/other.vcv\n"):
            self.assertFalse(rack_open._matches_recorded_identity(
                app, artifact, 1, log, rack_open._file_sha256(artifact)))

    def test_patch_rewrite_during_identity_check_abandons_focus(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge A\n")
        process = "41:Sat Aug  8 18:00:00 2026"
        selected_hash = rack_open._file_sha256(artifact)
        with mock.patch.object(rack_open, "_rack_process_identity",
                               return_value=process):
            rack_open._record_verified_open(
                app, artifact, 1, log, selected_hash)
        with temp, \
                mock.patch.object(rack_open, "_rack_process_identity",
                                  return_value=process), \
                mock.patch.object(rack_open, "_file_sha256",
                                  side_effect=[selected_hash, "changed"]):
            self.assertFalse(rack_open._matches_recorded_identity(
                app, artifact, 1, log, selected_hash))

    def test_stale_matching_load_does_not_bypass_different_latest_patch(self) -> None:
        temp, app, artifact, log = self.launch_fixture(
            artifact_name="a generated patch.vcv")
        pathlib.Path(log).write_text(
            f'Loading patch "{os.path.realpath(artifact)}"\n'
            "Creating module Forge A\n"
            "Loading patch /tmp/a different patch.vcv\n"
            "Creating module Forge B\n")
        evidence_attempt = mock.Mock(side_effect=[[], ["proved cold"]])
        with temp, \
                mock.patch.object(rack_open, "rack_running", return_value=True), \
                mock.patch.object(rack_open, "_focus") as focus, \
                mock.patch.object(rack_open, "_document_open") as opened, \
                mock.patch.object(rack_open, "_wait_for_evidence",
                                  evidence_attempt), \
                mock.patch.object(rack_open, "_quit") as quit_app, \
                mock.patch.object(rack_open, "_wait_stopped",
                                  return_value=True) as stopped:
            self.assertEqual(
                ["proved cold"], rack_open.open_patch(app, artifact, log))
        canonical = os.path.realpath(artifact)
        focus.assert_not_called()
        self.assertEqual(
            [mock.call(app, canonical), mock.call(app, canonical)],
            opened.call_args_list)
        quit_app.assert_called_once_with(app)
        stopped.assert_called_once_with(app, 15.0)

    def test_incomplete_existing_module_evidence_uses_document_handoff(self) -> None:
        temp, app, artifact, log = self.launch_fixture(module_count=2)
        pathlib.Path(log).write_text(
            f"Loading patch {os.path.realpath(artifact)}\n"
            "Creating module Forge A\n")
        with temp, \
                mock.patch.object(rack_open, "rack_running", return_value=True), \
                mock.patch.object(rack_open, "_focus") as focus, \
                mock.patch.object(rack_open, "_document_open") as opened, \
                mock.patch.object(rack_open, "_wait_for_evidence",
                                  return_value=["proved fresh"]), \
                mock.patch.object(rack_open, "_quit") as quit_app:
            self.assertEqual(
                ["proved fresh"], rack_open.open_patch(app, artifact, log))
        focus.assert_not_called()
        opened.assert_called_once_with(app, os.path.realpath(artifact))
        quit_app.assert_not_called()

    def test_different_patch_with_unsaved_dialog_fails_closed(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        pathlib.Path(log).write_text(
            "Loading patch /tmp/unsaved-current.vcv\n"
            "Creating module Forge A\n")
        with temp, \
                mock.patch.object(rack_open, "rack_running", return_value=True), \
                mock.patch.object(rack_open, "_focus") as focus, \
                mock.patch.object(rack_open, "_document_open") as opened, \
                mock.patch.object(rack_open, "_wait_for_evidence",
                                  return_value=[]), \
                mock.patch.object(rack_open, "_quit") as quit_app, \
                mock.patch.object(rack_open, "_wait_stopped",
                                  return_value=False) as stopped:
            with self.assertRaisesRegex(RuntimeError, "close/save Rack"):
                rack_open.open_patch(app, artifact, log)
        focus.assert_not_called()
        opened.assert_called_once_with(app, os.path.realpath(artifact))
        quit_app.assert_called_once_with(app)
        stopped.assert_called_once_with(app, 15.0)

    def test_warm_miss_retries_cold_after_nonzero_quit_race(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        events = []
        evidence_attempt = 0
        real_quit = rack_open._quit

        def opened_exact(selected_app, selected_patch):
            events.append(("open", selected_app, selected_patch))

        def evidence_exact(log_path, before, selected_patch, modules, timeout):
            nonlocal evidence_attempt
            evidence_attempt += 1
            events.append(("evidence", evidence_attempt, selected_patch))
            return [] if evidence_attempt == 1 else ["proved cold"]

        def quit_exact(selected_app):
            events.append(("quit", selected_app))
            real_quit(selected_app)

        def stopped_exact(selected_app, timeout):
            events.append(("stopped", selected_app, timeout))
            return True

        with temp, \
                mock.patch.object(rack_open, "rack_running", return_value=True), \
                mock.patch.object(rack_open, "_document_open",
                                  side_effect=opened_exact) as opened, \
                mock.patch.object(rack_open, "_wait_for_evidence",
                                  side_effect=evidence_exact), \
                mock.patch.object(rack_open, "_quit",
                                  side_effect=quit_exact), \
                mock.patch.object(rack_open, "_wait_stopped",
                                  side_effect=stopped_exact) as stopped, \
                mock.patch.object(rack_open.subprocess, "run",
                                  return_value=mock.Mock(returncode=1)) as run:
            self.assertEqual(
                ["proved cold"], rack_open.open_patch(app, artifact, log))
        canonical = os.path.realpath(artifact)
        self.assertEqual(
            [mock.call(app, canonical), mock.call(app, canonical)],
            opened.call_args_list)
        stopped.assert_called_once_with(app, 15.0)
        self.assertIn(
            mock.call(
                ["osascript", "-e",
                 'tell application "VCV Rack 2 Free" to quit'],
                check=False, stdout=rack_open.subprocess.DEVNULL,
                stderr=rack_open.subprocess.DEVNULL),
            run.call_args_list)
        self.assertEqual([
            ("open", app, canonical),
            ("evidence", 1, canonical),
            ("quit", app),
            ("stopped", app, 15.0),
            ("open", app, canonical),
            ("evidence", 2, canonical),
        ], events)

    def test_detector_error_after_warm_miss_never_opens_again(self) -> None:
        temp, app, artifact, log = self.launch_fixture()
        running = mock.Mock(side_effect=[True,
                                        RuntimeError("pgrep exit 2")])
        with temp, \
                mock.patch.object(rack_open, "rack_running", running), \
                mock.patch.object(rack_open, "_document_open") as opened, \
                mock.patch.object(rack_open, "_wait_for_evidence",
                                  return_value=[]), \
                mock.patch.object(rack_open, "_quit"):
            with self.assertRaisesRegex(RuntimeError, "pgrep exit 2"):
                rack_open.open_patch(app, artifact, log)
        opened.assert_called_once_with(app, os.path.realpath(artifact))


class GeneratedSlugSafety(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.pack = pathlib.Path(self.temp.name)
        (self.pack / "modules").mkdir()
        (self.pack / "src").mkdir()
        self.pack_patch = mock.patch.object(generate, "PACK", str(self.pack))
        self.pack_patch.start()

    def tearDown(self) -> None:
        self.pack_patch.stop()
        self.temp.cleanup()

    def write_manifest(self, filename: str, slug: str) -> pathlib.Path:
        path = self.pack / "modules" / filename
        path.write_text(json.dumps({"modules": [{"slug": slug}]}))
        return path

    def test_existing_manifest_path_is_never_truncated(self) -> None:
        path = self.write_manifest("vco.json", "VCO")
        before = path.read_bytes()

        with self.assertRaisesRegex(generate.ExistingModuleSlug,
                                    "manifest already exists"):
            generate._write_generated_module({"slug": "VCO"}, "replacement")

        self.assertEqual(path.read_bytes(), before)
        self.assertFalse((self.pack / "src" / "VCO.cpp").exists())

    def test_slug_identity_is_refused_across_manifest_filenames_and_case(self) -> None:
        path = self.write_manifest("legacy-name.json", "ExistingVoice")
        before = path.read_bytes()

        with self.assertRaisesRegex(generate.ExistingModuleSlug,
                                    "already declared"):
            generate._write_generated_module(
                {"slug": "EXISTINGVOICE"}, "replacement")

        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(list((self.pack / "src").iterdir()), [])

    def test_existing_source_or_symlink_is_never_followed(self) -> None:
        source = self.pack / "src" / "ORPHAN.cpp"
        source.write_text("keep me")
        with self.assertRaisesRegex(generate.ExistingModuleSlug,
                                    "source file already exists"):
            generate._write_generated_module({"slug": "ORPHAN"}, "replace")
        self.assertEqual(source.read_text(), "keep me")

        dangling = self.pack / "modules" / "linked.json"
        os.symlink(self.pack / "missing-target", dangling)
        with self.assertRaisesRegex(generate.ExistingModuleSlug,
                                    "manifest already exists"):
            generate._write_generated_module({"slug": "LINKED"}, "body")

    def test_unique_slug_writes_both_new_files(self) -> None:
        generate._write_generated_module(
            {"slug": "FRESHVOICE", "name": "Fresh Voice"}, "// dsp\n")

        manifest = json.loads(
            (self.pack / "modules" / "freshvoice.json").read_text())
        self.assertTrue(manifest["forge_generated"])
        self.assertEqual(manifest["modules"][0]["slug"], "FRESHVOICE")
        self.assertEqual((self.pack / "src" / "FRESHVOICE.cpp").read_text(),
                         "// dsp\n")

    def test_invalid_slug_never_constructs_or_escapes_output_paths(self) -> None:
        outside = self.pack.parent / "escaped.cpp"
        for slug in ("../escaped", "/tmp/escaped", "A/B", "A\\B", ".",
                     "MixedCase", "WITH SPACE", ""):
            with self.subTest(slug=slug):
                with self.assertRaisesRegex(generate.InvalidModuleSlug,
                                            "A-Z0-9 only"):
                    generate._write_generated_module(
                        {"slug": slug, "name": "Bad"}, "escape")
                self.assertEqual(list((self.pack / "modules").iterdir()), [])
                self.assertEqual(list((self.pack / "src").iterdir()), [])
                self.assertFalse(outside.exists())


class PromptBudgetSafety(unittest.TestCase):
    def test_repeated_defaults_use_the_contracts_compact_lossless_form(self) -> None:
        inventory = {"Fixture": {"name": "Fixture", "modules": {
            "Voice": {"name": "Voice", "params": [
                {"id": 3, "name": "Level", "min": 0.0, "max": 1.0,
                 "default": 0.5},
            ]},
        }}}

        rendered = patch.render_inventory(inventory)
        contract = pathlib.Path(patch.CONTRACT).read_text()

        self.assertIn("3=Level[0..1,d=0.5]", rendered)
        self.assertNotIn(", default ", rendered)
        self.assertIn("`d=` is that knob's default value", contract)


class BundledToolchainSafety(unittest.TestCase):
    def test_writable_developer_bundle_still_seeds_application_support(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp) / "Forge Modular.app" / "Contents" / "Resources"
            here = root / "tools" / "rack"
            pack = root / "examples" / "forge-modular"
            installed = pathlib.Path(temp) / "Application Support" / "Forge Modular"
            (pack / "modules").mkdir(parents=True)
            (pack / "src").mkdir()
            here.mkdir(parents=True)
            (here / "install_toolchain.sh").write_text("#!/bin/sh\n")
            target = installed / "tools" / "rack" / "generate.py"
            target.parent.mkdir(parents=True)
            target.write_text("# installed\n")
            completed = mock.Mock(returncode=0, stdout="installed\n", stderr="")

            with mock.patch.object(generate, "ROOT", str(root)), \
                    mock.patch.object(generate, "HERE", str(here)), \
                    mock.patch.object(generate, "PACK", str(pack)), \
                    mock.patch.object(generate, "INSTALLED_HOME", str(installed)), \
                    mock.patch.object(generate.subprocess, "run",
                                      return_value=completed) as run, \
                    mock.patch.object(generate.os, "execve",
                                      side_effect=RuntimeError("re-executed")) as execve, \
                    mock.patch.dict(os.environ,
                                    {"FORGE_TOOLCHAIN_SEEDED": ""}):
                with self.assertRaisesRegex(RuntimeError, "re-executed"):
                    generate.ensure_writable_toolchain(
                        ["generate.py", "clock"])

            run.assert_called_once_with(
                ["/bin/bash", str(here / "install_toolchain.sh")],
                capture_output=True, text=True)
            self.assertEqual(sys.executable, execve.call_args.args[0])
            self.assertEqual(
                [sys.executable, str(target), "clock"],
                execve.call_args.args[1])
            self.assertEqual(
                "1", execve.call_args.args[2]["FORGE_TOOLCHAIN_SEEDED"])

    def test_writable_non_bundle_toolchain_stays_in_place(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp) / "toolchain"
            pack = root / "examples" / "forge-modular"
            (pack / "modules").mkdir(parents=True)
            (pack / "src").mkdir()
            with mock.patch.object(generate, "ROOT", str(root)), \
                    mock.patch.object(generate, "PACK", str(pack)), \
                    mock.patch.object(generate.subprocess, "run") as run:
                generate.ensure_writable_toolchain(["generate.py", "clock"])
            run.assert_not_called()


class ExplicitRequestContract(unittest.TestCase):
    def setUp(self) -> None:
        self.prompt = (
            "Create a 6HP clock. RATE -3..3 default 0; WIDTH 0.05..0.95 "
            "default 0.5; normalled RATE CV input; RESET input; CLOCK 10V "
            "output; PHASE 0..10V output.")
        self.module = {
            "hp": 6,
            "params": [
                {"name": "RATE", "min_value": -3, "max_value": 3,
                 "default_value": 0},
                {"name": "WIDTH", "min_value": 0.05, "max_value": 0.95,
                 "default_value": 0.5},
            ],
            "inputs": [
                {"name": "RATE CV", "normal_volts": 0},
                {"name": "RESET"},
            ],
            "outputs": [
                {"name": "CLOCK", "role": "Clock"},
                {"name": "PHASE", "role": "Cv"},
            ],
        }

    def test_exact_manifest_satisfies_prompt_derived_contract(self) -> None:
        self.assertEqual([], generate.module_intent_problems(
            self.prompt, self.module))

    def test_wrong_or_missing_explicit_facts_fail_closed(self) -> None:
        broken = json.loads(json.dumps(self.module))
        broken["hp"] = 8
        broken["params"][1]["default_value"] = 0.25
        broken["inputs"] = broken["inputs"][:1]
        broken["outputs"][0]["role"] = "Gate"
        broken["outputs"] = broken["outputs"][:1]
        problems = generate.module_intent_problems(self.prompt, broken)
        self.assertTrue(any("requested 6HP" in problem for problem in problems))
        self.assertTrue(any("WIDTH requested range/default" in problem
                            for problem in problems))
        self.assertIn("requested RESET input is missing", problems)
        self.assertTrue(any("CLOCK must declare role Clock" in problem
                            for problem in problems))
        self.assertIn("requested output PHASE is missing", problems)


class FailedGenerationTransaction(unittest.TestCase):
    def test_restore_covers_every_emitter_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            pack = pathlib.Path(root)
            for sub in ("modules", "src", "res"):
                (pack / sub).mkdir()
                (pack / sub / "original.txt").write_text(sub)
            (pack / "plugin.json").write_text('{"modules": []}\n')
            before = {
                path.relative_to(pack): path.read_bytes()
                for path in pack.rglob("*") if path.is_file()
            }

            transaction = generate.PackSnapshot(pack)
            (pack / "modules" / "new.json").write_text("new")
            (pack / "src" / "generated_modules.hpp").write_text("new")
            (pack / "res" / "NEW.svg").write_text("new")
            (pack / "plugin.json").write_text('{"modules": ["NEW"]}\n')
            transaction.restore()
            transaction.close()

            after = {
                path.relative_to(pack): path.read_bytes()
                for path in pack.rglob("*") if path.is_file()
            }
            self.assertEqual(before, after)

    def test_attempt_artifacts_are_immutable_and_exact(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            path = generate.retain_attempt_text(root, 1, "model-response",
                                                "exact answer\n")
            self.assertEqual("exact answer\n", pathlib.Path(path).read_text())
            with self.assertRaises(FileExistsError):
                generate.retain_attempt_text(root, 1, "model-response",
                                             "replacement")

    def test_unexpected_exception_restores_pack_byte_for_byte(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            pack = pathlib.Path(root)
            for sub in ("modules", "src", "res"):
                (pack / sub).mkdir()
                (pack / sub / "original.bin").write_bytes(sub.encode())
            (pack / "plugin.json").write_text('{"modules": []}\n')
            before = {path.relative_to(pack): path.read_bytes()
                      for path in pack.rglob("*") if path.is_file()}

            def explode(_argv, _resources):
                (pack / "modules" / "orphan.json").write_text("orphan")
                (pack / "res" / "orphan.svg").write_text("orphan")
                (pack / "plugin.json").write_text('{"modules": ["ORPHAN"]}\n')
                raise RuntimeError("unexpected packaging failure")

            with mock.patch.object(generate, "PACK", str(pack)), \
                    mock.patch.object(generate, "_main", side_effect=explode):
                with self.assertRaisesRegex(RuntimeError,
                                            "unexpected packaging failure"):
                    generate.main(["generate.py", "fixture"])

            after = {path.relative_to(pack): path.read_bytes()
                     for path in pack.rglob("*") if path.is_file()}
            self.assertEqual(before, after)


class ZeroModelReplay(unittest.TestCase):
    def test_failed_provider_partial_response_is_retained(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            pack = pathlib.Path(root) / "pack"
            (pack / "modules").mkdir(parents=True)
            (pack / "src").mkdir()
            attempts = pathlib.Path(root) / "attempts"
            with mock.patch.dict(
                    os.environ, {"FORGE_ATTEMPT_DIR": str(attempts)}), \
                    mock.patch.object(generate, "PACK", str(pack)), \
                    mock.patch.object(generate.fetch_sdk, "compiler_missing",
                                      return_value=None), \
                    mock.patch.object(generate, "preflight"), \
                    mock.patch.object(generate, "ensure_writable_toolchain"), \
                    mock.patch.object(generate, "resolve_sdk", return_value="/sdk"), \
                    mock.patch.object(
                        generate, "ask_model",
                        side_effect=generate.ModelCallFailed(
                            "provider failed", "partial paid response")):
                with self.assertRaisesRegex(SystemExit, "provider failed"):
                    generate.main(["generate.py", "clock generator"])

            self.assertEqual(
                "partial paid response",
                (attempts / "attempt01-model-response.txt").read_text())

    def test_unusable_evidence_root_fails_before_module_provider_call(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            pack = pathlib.Path(root) / "pack"
            (pack / "modules").mkdir(parents=True)
            (pack / "src").mkdir()
            blocked = pathlib.Path(root) / "not-a-directory"
            blocked.write_text("occupied")
            with mock.patch.dict(
                    os.environ, {"FORGE_ATTEMPT_DIR": str(blocked)}), \
                    mock.patch.object(generate, "PACK", str(pack)), \
                    mock.patch.object(generate.fetch_sdk, "compiler_missing",
                                      return_value=None), \
                    mock.patch.object(generate, "preflight"), \
                    mock.patch.object(generate, "ensure_writable_toolchain"), \
                    mock.patch.object(generate, "resolve_sdk", return_value="/sdk"), \
                    mock.patch.object(
                        generate, "ask_model",
                        side_effect=AssertionError("provider call is forbidden")):
                with self.assertRaisesRegex(
                        SystemExit, "cannot reserve generation evidence"):
                    generate.main(["generate.py", "clock generator"])

    def test_invalid_model_slug_fails_cleanly_and_restores_pack(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            pack = pathlib.Path(root) / "pack"
            (pack / "modules").mkdir(parents=True)
            (pack / "src").mkdir()
            response = pathlib.Path(root) / "response.txt"
            response.write_text(
                "```json manifest\n"
                '{"slug":"BAD/SLUG","name":"Bad","hp":4,'
                '"params":[],"inputs":[],"outputs":[],"tags":[]}'
                "\n```\n```cpp dsp\n// must not be written\n```\n")
            with mock.patch.object(generate, "PACK", str(pack)), \
                    mock.patch.object(generate.fetch_sdk, "compiler_missing",
                                      return_value=None), \
                    mock.patch.object(generate, "preflight"), \
                    mock.patch.object(generate, "ensure_writable_toolchain"), \
                    mock.patch.object(generate, "resolve_sdk", return_value="/sdk"):
                with self.assertRaisesRegex(
                        SystemExit, "gave up after 1 attempts; pack restored unchanged"):
                    generate.main([
                        "generate.py", "clock generator", "--response-file",
                        str(response), "--install-dir", str(pathlib.Path(root) / "install")])

            self.assertEqual([], list((pack / "modules").iterdir()))
            self.assertEqual([], list((pack / "src").iterdir()))
            self.assertFalse((pathlib.Path(root) / "install").exists())

    def test_saved_response_runs_normal_module_pipeline_without_provider(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            pack = pathlib.Path(root) / "pack"
            (pack / "modules").mkdir(parents=True)
            (pack / "src").mkdir()
            response = pathlib.Path(root) / "response.txt"
            response.write_text(
                "```json manifest\n"
                '{"modules":[{"slug":"REPLAYVCA","name":"Replay VCA",'
                '"hp":4,"params":[],"inputs":[],"outputs":[],"tags":[]}]}'
                "\n```\n```cpp dsp\n"
                "#include <pulp/signal/vca.hpp>\n"
                "pulp::signal::VcaT<float> amplifier;\n"
                "```\n")
            with mock.patch.object(generate, "PACK", str(pack)), \
                    mock.patch.object(generate.fetch_sdk, "compiler_missing",
                                      return_value=None), \
                    mock.patch.object(generate, "preflight"), \
                    mock.patch.object(generate, "ensure_writable_toolchain"), \
                    mock.patch.object(generate, "resolve_sdk", return_value="/sdk"), \
                    mock.patch.object(generate, "ask_model", side_effect=AssertionError(
                        "provider must not run during response replay")), \
                    mock.patch.object(generate, "run_emitter",
                                      return_value=(True, "validated")), \
                    mock.patch.object(generate, "compile_all",
                                      return_value=(True, "/tmp/plugin.dylib", [])), \
                    mock.patch.object(generate, "run_behaviour_gate",
                                      return_value=(True, "gate passed")), \
                    mock.patch.object(generate, "install",
                                      return_value="/tmp/replay.vcvplugin") as install:
                rc = generate.main([
                    "generate.py", "VCA with gain CV and audio input", "--retries", "9",
                    "--response-file", str(response),
                    "--install-dir", str(pathlib.Path(root) / "install")])

            self.assertEqual(0, rc)
            self.assertTrue((pack / "modules" / "replayvca.json").is_file())
            self.assertTrue((pack / "src" / "REPLAYVCA.cpp").is_file())
            self.assertTrue((pack / "patches" / "replayvca.vcv").is_file())
            install.assert_called_once_with(
                "/tmp/plugin.dylib", str(pathlib.Path(root) / "install"))


class PulpDspUseGate(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.pack = pathlib.Path(self.temp.name)
        (self.pack / "src").mkdir()
        self.pack_patch = mock.patch.object(generate, "PACK", str(self.pack))
        self.pack_patch.start()

    def tearDown(self) -> None:
        self.pack_patch.stop()
        self.temp.cleanup()

    def test_include_and_comment_do_not_count_as_using_pulp_dsp(self) -> None:
        source = self.pack / "src" / "INCLUDEONLY.cpp"
        source.write_text(
            "#include <pulp/signal/vca.hpp>\n"
            "// pulp::signal::VcaT<float> only_in_a_comment;\n")
        ok, message = generate.check_uses_pulp_dsp(
            "INCLUDEONLY", {}, "VCA with gain CV and audio input")
        self.assertFalse(ok)
        self.assertIn("does not use the primary Pulp DSP capability", message)

        source.write_text(
            "#include <pulp/signal/vca.hpp>\n"
            "pulp::signal::VcaT<float> amplifier;\n")
        ok, message = generate.check_uses_pulp_dsp(
            "INCLUDEONLY", {}, "VCA with gain CV and audio input")
        self.assertTrue(ok)
        self.assertIn("vca", message)


if __name__ == "__main__":
    unittest.main()
