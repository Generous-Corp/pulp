#!/usr/bin/env python3
"""Self-test census_profiles.py: every profile is walked, a stale sibling is caught, a cache cannot hide one.

The controls matter more than the cases: the digest must NOT move for an edit the
census cannot see (a .cpp), or it would force a regeneration on every PR, and a
stale sibling profile must be reported by the one-command check while the single
local-tree check still passes, which is the gap this script closes.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import platform
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import census_profiles as cp  # noqa: E402
import gates_script_inputs as gsi  # noqa: E402

HOST = f"{platform.system().lower()}-{platform.machine().lower()}"
KEY_ALL, KEY_NONE = f"{HOST}-ausdk+js+vst3", f"{HOST}-js"


def sh(cwd: Path, *cmd: str) -> str:
    return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True, encoding="utf-8").stdout


def features(sdk: str, gpu: str = "OFF") -> dict:
    return {"PULP_HAS_SKIA": gpu, "PULP_ENABLE_JS": "ON", "PULP_HAS_VST3": sdk, "PULP_HAS_AUSDK": sdk}


def census(extra: dict | None = None) -> str:
    profiles = {KEY_ALL: {"features": features("ON")}, KEY_NONE: {"features": features("OFF")},
                "plan9-mips-none": {"features": features("OFF")}, **(extra or {})}
    return json.dumps({"profiles": profiles})


def fake_cmake(path: Path, record: Path, features_json: dict) -> None:
    path.write_text(f"""#!/bin/sh
src=""; build=""
while [ $# -gt 0 ]; do
  case "$1" in -S) src="$2"; shift ;; -B) build="$2"; shift ;; esac
  shift
done
echo "$src" > "{record}"
mkdir -p "$build"
cat > "$build/consumption-census-facts.json" <<'EOF'
{json.dumps({"facts_schema": 1, "system_name": platform.system(), "system_processor": platform.machine(),
             "features": features_json})}
EOF
""", encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


class CensusProfilesTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name).resolve() / "repo"
        for rel, text in {
            "docs/status/consumption-profiles.json": census(),
            "CMakeLists.txt": "project(x)\n",
            "core/CMakeLists.txt": "add_library(a)\n",
            "core/include/a.hpp": "",
            "core/src/a.cpp": "int a;\n",
            "tools/scripts/consumption_census.py": "# census\n",
            "tools/scripts/census_profiles.py": "# script\n",
        }.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        for sdk in ("vst3sdk", "AudioUnitSDK"):
            (self.root / "external" / sdk).mkdir(parents=True)
        (self.root / "external" / "choc").mkdir()
        (self.root / "build-old").mkdir()
        sh(self.root, "git", "init", "-q")
        sh(self.root, "git", "add", "-A")

    def test_profiles_split_by_platform(self) -> None:
        specs, other = cp.committed_specs(self.root)
        self.assertEqual([s.key for s in specs], [KEY_ALL, KEY_NONE])
        self.assertEqual(other, ["plan9-mips-none"])

    def test_spec_maps_features_to_sdks_and_cmake_options(self) -> None:
        specs, _ = cp.committed_specs(self.root)
        self.assertEqual(specs[0].sdks(), {"vst3sdk", "AudioUnitSDK"})
        self.assertEqual(specs[1].sdks(), set())
        gpu = cp.Spec(KEY_NONE, features("OFF", gpu="ON"))
        self.assertIn("-DPULP_ENABLE_GPU=ON", gpu.cmake_args())
        self.assertIn("-DPULP_ENABLE_GPU=OFF", specs[1].cmake_args())

    def test_overlay_omits_only_the_sdks_a_profile_does_not_record(self) -> None:
        dest = Path(self._tmp.name) / "overlay"
        dest.mkdir()
        cp.make_overlay(self.root, dest, set())
        self.assertFalse((dest / "external" / "vst3sdk").exists())
        self.assertFalse((dest / "external" / "AudioUnitSDK").exists())
        self.assertTrue((dest / "external" / "choc").exists())
        self.assertTrue((dest / "core").is_symlink())
        self.assertFalse((dest / "build-old").exists(), "a build dir must never be part of the source view")
        self.assertTrue((self.root / "external" / "vst3sdk").is_dir(), "the checkout itself is untouched")

    def test_a_submodule_is_an_empty_directory_so_git_status_still_works(self) -> None:
        (self.root / "planning").mkdir()
        sh(self.root, "git", "update-index", "--add", "--cacheinfo", "160000,1234567890123456789012345678901234567890,planning")
        dest = Path(self._tmp.name) / "overlay"
        dest.mkdir()
        cp.make_overlay(self.root, dest, set())
        self.assertTrue((dest / "planning").is_dir() and not (dest / "planning").is_symlink())
        self.assertTrue((dest / "core").is_symlink(), "an ordinary directory is still a symlink")

    def test_the_overlay_lives_in_the_worktree_under_a_build_name(self) -> None:
        # CMake refuses a checkout under the system temp dir, which is where the first version put it.
        with cp.scratch_source_dir(self.root) as scratch:
            path = Path(scratch)
            self.assertEqual(path.parent, self.root)
            self.assertTrue(path.name.startswith("build"))
            self.assertFalse(str(path).startswith(tempfile.gettempdir()))
            overlay = cp.make_overlay(self.root, path, set())
            self.assertFalse((overlay / path.name).exists(), "the overlay must not contain itself")
        self.assertFalse(path.exists(), "removed when the profile is done")

    def test_control_overlay_keeps_the_sdks_it_is_told_to(self) -> None:
        dest = Path(self._tmp.name) / "overlay"
        dest.mkdir()
        cp.make_overlay(self.root, dest, {"vst3sdk"})
        self.assertTrue((dest / "external" / "vst3sdk").exists())
        self.assertFalse((dest / "external" / "AudioUnitSDK").exists())

    def test_digest_moves_for_cmake_and_header_edits(self) -> None:
        base = cp.inputs_digest(self.root)
        original = (self.root / "core/CMakeLists.txt").read_text(encoding="utf-8")
        (self.root / "core/CMakeLists.txt").write_text("add_library(b)\n", encoding="utf-8")
        self.assertNotEqual(cp.inputs_digest(self.root), base)
        (self.root / "core/CMakeLists.txt").write_text(original, encoding="utf-8")
        self.assertEqual(cp.inputs_digest(self.root), base)
        (self.root / "core/include/b.hpp").write_text("", encoding="utf-8")
        sh(self.root, "git", "add", "core/include/b.hpp")
        self.assertNotEqual(cp.inputs_digest(self.root), base)

    def test_a_profile_digest_moves_only_with_its_own_block_or_the_header_lists(self) -> None:
        document = json.loads(census())
        common = cp.inputs_digest(self.root)
        base = {k: cp.profile_digest(common, document, k) for k in (KEY_ALL, KEY_NONE)}
        edited = json.loads(census())
        edited["profiles"][KEY_NONE]["features"]["PULP_ENABLE_JS"] = "OFF"
        self.assertNotEqual(cp.profile_digest(common, edited, KEY_NONE), base[KEY_NONE])
        self.assertEqual(cp.profile_digest(common, edited, KEY_ALL), base[KEY_ALL],
                         "a sibling's cached result must survive an edit it cannot see")
        edited["public_headers_by_root"] = {"core/include": ["a.hpp"]}
        self.assertNotEqual(cp.profile_digest(common, edited, KEY_ALL), base[KEY_ALL])

    def test_control_digest_ignores_an_edit_the_census_cannot_see(self) -> None:
        base = cp.inputs_digest(self.root)
        (self.root / "core/src/a.cpp").write_text("int a = 2;\n", encoding="utf-8")
        self.assertEqual(cp.inputs_digest(self.root), base)

    def configure(self, spec: cp.Spec, cmake_features: dict, sdks: bool = True) -> tuple[str, str]:
        record = Path(self._tmp.name) / "source-arg"
        script = Path(self._tmp.name) / "cmake"
        fake_cmake(script, record, cmake_features)
        build, scratch = Path(self._tmp.name) / "b", Path(self._tmp.name) / "s"
        build.mkdir(exist_ok=True)
        scratch.mkdir(exist_ok=True)
        with mock.patch.dict(os.environ, {"CMAKE": str(script), "PULP_CONFIGURE_ONLY_GOVERNOR": ""}), \
                contextlib.redirect_stderr(io.StringIO()):
            why, _ = cp.configure_profile(self.root, spec, build, scratch)
        return why, record.read_text(encoding="utf-8").strip() if record.exists() else ""

    def test_sdk_less_profile_is_configured_from_the_overlay(self) -> None:
        none = cp.Spec(KEY_NONE, features("OFF"))
        why, source = self.configure(none, features("OFF"))
        self.assertEqual(why, "")
        self.assertNotEqual(Path(source), self.root)
        self.assertTrue(Path(source).name == "s")

    def test_force_overlay_configures_even_a_buildable_profile_from_the_view(self) -> None:
        record = Path(self._tmp.name) / "source-arg"
        script = Path(self._tmp.name) / "cmake"
        fake_cmake(script, record, features("ON"))
        build, scratch = Path(self._tmp.name) / "b", Path(self._tmp.name) / "s"
        build.mkdir()
        scratch.mkdir()
        with mock.patch.dict(os.environ, {"CMAKE": str(script), "PULP_CONFIGURE_ONLY_GOVERNOR": ""}), \
                contextlib.redirect_stderr(io.StringIO()):
            why, source = cp.configure_profile(self.root, cp.Spec(KEY_ALL, features("ON")), build, scratch,
                                               force_overlay=True)
        self.assertEqual(why, "")
        self.assertNotEqual(source, self.root)
        self.assertTrue((source / "external" / "vst3sdk").exists(), "the profile's SDKs are still present")

    def test_sdk_profile_is_configured_from_the_checkout(self) -> None:
        why, source = self.configure(cp.Spec(KEY_ALL, features("ON")), features("ON"))
        self.assertEqual(why, "")
        self.assertEqual(Path(source), self.root)

    def test_the_generator_is_given_the_tree_cmake_was_pointed_at_and_the_real_census(self) -> None:
        # An overlay profile records include roots under the overlay; read against the real
        # checkout they all look moved, which is the drift the first version reported.
        overlay = Path(self._tmp.name) / "overlay"
        cmd = cp.census_cmd(self.root, Path("/b"), "--check", overlay)
        self.assertEqual(cmd[cmd.index("--source-root") + 1], str(overlay))
        self.assertEqual(cmd[cmd.index("--census") + 1], str(self.root / "docs/status/consumption-profiles.json"))
        self.assertEqual(cmd[-1], "--check")

    def test_a_configure_that_lands_on_another_key_is_not_configurable(self) -> None:
        why, _ = self.configure(cp.Spec(f"{HOST}-js+skia", features("OFF", gpu="ON")), features("OFF"))
        self.assertIn("configures that feature set as", why)

    def test_missing_sdk_names_the_bootstrap_command(self) -> None:
        import shutil
        shutil.rmtree(self.root / "external" / "vst3sdk")
        why, _ = self.configure(cp.Spec(KEY_ALL, features("ON")), features("ON"))
        self.assertIn("vst3sdk", why)
        self.assertIn("./setup.sh --deps-only --non-interactive", why)

    def run_main(self, *args: str, results: dict | None = None) -> tuple[int, str, list[str]]:
        calls: list[str] = []

        def fake_process(root: Path, spec: cp.Spec, mode: str, force_overlay: bool = False) -> tuple[str, str]:
            calls.append(spec.key)
            return (results or {}).get(spec.key, ("ok", "consumption_census_verified=true"))

        err = io.StringIO()
        with mock.patch.object(cp, "process", fake_process), contextlib.redirect_stderr(err):
            rc = cp.main(["census_profiles.py", "--repo-root", str(self.root), *args])
        return rc, err.getvalue(), calls

    def test_check_walks_every_profile_and_a_stale_sibling_fails_with_the_one_command(self) -> None:
        rc, err, calls = self.run_main("--check", results={KEY_NONE: ("drift", "Pulp::host closure changed")})
        self.assertEqual(rc, 1, err)
        self.assertEqual(calls, [KEY_ALL, KEY_NONE])
        self.assertIn("python3 tools/scripts/census_profiles.py --write", err)
        self.assertIn(f"drift            {KEY_NONE}", err)
        self.assertIn("plan9-mips-none: carried through untouched", err)

    def test_control_all_current_exits_zero(self) -> None:
        rc, err, calls = self.run_main("--check")
        self.assertEqual(rc, 0, err)
        self.assertEqual(calls, [KEY_ALL, KEY_NONE])

    def test_not_configurable_is_not_a_pass(self) -> None:
        rc, err, _ = self.run_main("--check", results={KEY_ALL: ("not-configurable", "no Skia library")})
        self.assertEqual(rc, 3, err)
        self.assertIn(f"NOT CHECKED here: {KEY_ALL}", err)

    def test_drift_outranks_not_configurable(self) -> None:
        rc, _, _ = self.run_main("--check", results={KEY_ALL: ("not-configurable", "x"), KEY_NONE: ("drift", "y")})
        self.assertEqual(rc, 1)

    def test_a_tampered_block_is_rechecked_while_its_verified_siblings_stay_cached(self) -> None:
        self.assertEqual(self.run_main("--check")[0], 0)
        document = json.loads((self.root / "docs/status/consumption-profiles.json").read_text(encoding="utf-8"))
        document["profiles"][KEY_NONE]["features"]["PULP_ENABLE_JS"] = "OFF"
        (self.root / "docs/status/consumption-profiles.json").write_text(json.dumps(document), encoding="utf-8")
        _, _, calls = self.run_main("--check", "--cached")
        self.assertEqual(calls, [KEY_NONE])

    def test_cached_skips_a_verified_profile_and_a_later_edit_invalidates_it(self) -> None:
        self.assertEqual(self.run_main("--check")[0], 0)
        rc, _, calls = self.run_main("--check", "--cached")
        self.assertEqual((rc, calls), (0, []), "a profile verified for these inputs must not be configured again")
        (self.root / "core/CMakeLists.txt").write_text("add_library(changed)\n", encoding="utf-8")
        _, _, calls = self.run_main("--check", "--cached")
        self.assertEqual(calls, [KEY_ALL, KEY_NONE])

    def test_a_failed_profile_is_never_cached(self) -> None:
        self.run_main("--check", results={KEY_NONE: ("drift", "y")})
        _, _, calls = self.run_main("--check", "--cached")
        self.assertEqual(calls, [KEY_NONE])

    def test_write_clears_the_cache(self) -> None:
        self.run_main("--check")
        self.run_main("--write")
        _, _, calls = self.run_main("--check", "--cached")
        self.assertEqual(calls, [KEY_ALL, KEY_NONE])

    def test_skip_names_a_profile_the_caller_already_verified(self) -> None:
        _, _, calls = self.run_main("--check", "--skip", KEY_ALL)
        self.assertEqual(calls, [KEY_NONE])


class GatesIntegrationTests(unittest.TestCase):
    def check(self, rc: int, out: str = "", key: str | None = "k", env: dict | None = None) -> tuple[int, str, list]:
        seen: list = []

        def fake_run_tool(cmd: list[str], root: Path) -> tuple[int, str]:
            seen.append(cmd)
            return rc, out

        err = io.StringIO()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "tools/scripts").mkdir(parents=True)
            (root / "tools/scripts/census_profiles.py").write_text("", encoding="utf-8")
            with mock.patch.object(gsi, "run_tool", fake_run_tool), \
                    mock.patch.dict(os.environ, env or {}), contextlib.redirect_stdout(err):
                result = gsi.check_other_profiles(root, key)
        return result, err.getvalue(), seen

    def test_stale_sibling_fails_the_gate_with_the_write_command(self) -> None:
        result, err, seen = self.check(1, "drift darwin-x")
        self.assertEqual(result, 1)
        self.assertIn("census_profiles.py --write", err)
        self.assertIn("--skip", seen[0])
        self.assertIn("k", seen[0])

    def test_control_current_siblings_pass_silently(self) -> None:
        result, err, _ = self.check(0, "census_profiles: 2 profile(s) current")
        self.assertEqual(result, 0)
        self.assertNotIn("DRIFT", err)

    def test_unconfigurable_sibling_is_listed_not_checked(self) -> None:
        result, err, _ = self.check(3, "NOT CHECKED here: x")
        self.assertEqual(result, 0)
        self.assertIn("NOT CHECKED locally: consumption-census-drift (other profiles", err)

    def test_local_only_opt_out_is_listed_not_checked(self) -> None:
        result, err, seen = self.check(0, env={"PULP_GATES_CENSUS_LOCAL_ONLY": "1"})
        self.assertEqual((result, seen), (0, []))
        self.assertIn("NOT CHECKED locally", err)


if __name__ == "__main__":
    unittest.main()
