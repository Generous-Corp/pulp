"""Script-level checks for the desktop Skia source fallback (tools/build-skia.sh).

Each case copies the script into a scratch Pulp root and drives it against
local git repositories standing in for Skia, Dawn, and skia-builder. The Skia
milestone branch has moved past the manifest pin, which is the normal state of
a Chromium release branch, so a fallback that compares or builds the branch
head cannot pass.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BRANCH = "chrome/m999"

FAKE_BUILDER = r'''
import argparse
import os
import pathlib
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("platform")
parser.add_argument("-branch", default="main")
parser.add_argument("-revision", help="Immutable Skia commit to build")
parser.add_argument("--shallow", action="store_true")
args = parser.parse_args()

# Same layout as skia-builder: everything lives under build/.
base = pathlib.Path(__file__).resolve().parent / "build"
base.mkdir(exist_ok=True)
with (base.parent / "invocations.log").open("a", encoding="utf-8") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")


def git(*command, cwd=None):
    return subprocess.run(
        ["git", *command], cwd=cwd, check=True, capture_output=True, text=True,
        encoding="utf-8",
    ).stdout.strip()


src = base / "src" / "skia"
if not src.exists():
    git("clone", "--branch", args.branch, os.environ["FAKE_SKIA_URL"], str(src))
if args.revision and os.environ.get("FAKE_BUILDER_IGNORES_REVISION") != "1":
    git("fetch", "--depth", "1", "origin", args.revision, cwd=src)
    git("checkout", "--detach", args.revision, cwd=src)
else:
    git("fetch", "origin", args.branch, cwd=src)
    git("checkout", "--detach", "origin/" + args.branch, cwd=src)

# Stand-in for git-sync-deps: the Skia revision's DEPS selects Dawn.
dawn = src / "third_party" / "externals" / "dawn"
if not dawn.exists():
    git("clone", os.environ["FAKE_DAWN_URL"], str(dawn))
git("checkout", "--detach", (src / "DAWN_PIN").read_text(encoding="utf-8").strip(), cwd=dawn)

(base / "include").mkdir(parents=True, exist_ok=True)
(base / "include" / "SkFake.h").write_text("// fake\n", encoding="utf-8")
(base / "share").mkdir(parents=True, exist_ok=True)
(base / "share" / "icudtl.dat").write_text("icu\n", encoding="utf-8")
lib = base / (args.platform + "-gpu") / "lib" / "Release" / "arm64"
lib.mkdir(parents=True, exist_ok=True)
(lib / "libskia.a").write_text(git("rev-parse", "HEAD", cwd=src) + "\n", encoding="utf-8")
if os.environ.get("FAKE_UNREADABLE_OUTPUT") == "1":
    unreadable = lib / "libdawn_combined.a"
    unreadable.write_text("dawn\n", encoding="utf-8")
    unreadable.chmod(0)
'''

# A builder revision from before skia-builder accepted an exact commit.
LEGACY_BUILDER = r'''
import argparse
import pathlib

parser = argparse.ArgumentParser()
parser.add_argument("platform")
parser.add_argument("-branch", default="main")
parser.add_argument("--shallow", action="store_true")
parser.parse_args()
(pathlib.Path(__file__).resolve().parent / "invocations.log").write_text("ran\n", encoding="utf-8")
'''


class SkiaSourceFallbackPinTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="skia-source-pin-")
        self.tmp = Path(self._temporary.name)
        gitconfig = self.tmp / "gitconfig"
        gitconfig.write_text(
            "[user]\n\tname = Pin Test\n\temail = pin-test@example.invalid\n"
            "[uploadpack]\n\tallowAnySHA1InWant = true\n"
            "[protocol \"file\"]\n\tallow = always\n"
            "[advice]\n\tdetachedHead = false\n",
            encoding="utf-8",
        )
        self.env = os.environ.copy()
        for key in [k for k in self.env if k.startswith("GIT_") or k.startswith("SKIA_")]:
            del self.env[key]
        self.env.pop("DAWN_EXPECTED_COMMIT", None)
        self.env.pop("PULP_SKIA_DRY_RUN", None)
        self.env.update({"GIT_CONFIG_GLOBAL": str(gitconfig), "GIT_CONFIG_NOSYSTEM": "1"})

        dawn = self._repo("dawn", "main")
        self.dawn_pinned = self._commit(dawn, "README", "dawn one\n")
        self.dawn_newer = self._commit(dawn, "README", "dawn two\n")

        skia = self._repo("skia", BRANCH)
        self.skia_pinned = self._commit(skia, "DAWN_PIN", self.dawn_pinned + "\n")
        # The milestone branch keeps moving after the release pin.
        self.skia_head = self._commit(skia, "DAWN_PIN", self.dawn_newer + "\n")

        self.env.update(
            {
                "FAKE_SKIA_URL": skia.as_uri(),
                "FAKE_DAWN_URL": dawn.as_uri(),
                # Read only by a branch-head comparison; it must not decide anything.
                "SKIA_SOURCE_URL": skia.as_uri(),
            }
        )

        self.pulp = self.tmp / "pulp"
        (self.pulp / "tools" / "deps").mkdir(parents=True)
        shutil.copy2(ROOT / "tools" / "build-skia.sh", self.pulp / "tools" / "build-skia.sh")

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def _git(self, *command: str, cwd: Path) -> str:
        return subprocess.run(
            ["git", *command],
            cwd=cwd,
            env=self.env,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
        ).stdout.strip()

    def _repo(self, name: str, branch: str) -> Path:
        path = self.tmp / name
        path.mkdir()
        self._git("init", "-q", "-b", branch, cwd=path)
        return path

    def _commit(self, repo: Path, filename: str, content: str) -> str:
        (repo / filename).write_text(content, encoding="utf-8")
        self._git("add", filename, cwd=repo)
        self._git("commit", "-q", "-m", "update " + filename, cwd=repo)
        return self._git("rev-parse", "HEAD", cwd=repo)

    def _builder(self, source: str) -> str:
        builder = self._repo("builder", "main")
        return self._commit(builder, "build-skia.py", source)

    def _manifest(self, builder_ref: str, dawn: str | None = None) -> None:
        manifest = {
            "dependencies": [
                {
                    "name": "Skia",
                    "determinism": {
                        "skia_branch": BRANCH,
                        "skia_commit": self.skia_pinned,
                        "built_dawn": dawn or self.dawn_pinned,
                        "skia_builder_ref": builder_ref,
                    },
                }
            ]
        }
        (self.pulp / "tools" / "deps" / "manifest.json").write_text(
            json.dumps(manifest), encoding="utf-8"
        )

    def _run(self, **extra: str) -> subprocess.CompletedProcess[str]:
        env = dict(self.env)
        env["SKIA_BUILDER_URL"] = (self.tmp / "builder").as_uri()
        env.update(extra)
        return subprocess.run(
            ["bash", str(self.pulp / "tools" / "build-skia.sh"), "mac"],
            cwd=self.pulp,
            env=env,
            text=True,
            encoding="utf-8",
            capture_output=True,
            check=False,
        )

    @property
    def published(self) -> Path:
        return self.pulp / "external" / "skia-build" / "build"

    @property
    def builder_out(self) -> Path:
        return self.pulp / "external" / "skia-builder" / "build"

    @property
    def output_lib(self) -> Path:
        return self.published / "mac-gpu" / "lib" / "Release" / "arm64" / "libskia.a"

    def _plant(self, path: Path, content: str) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def _previous_generation(self) -> list[Path]:
        """A generation published earlier, which a new publish must replace whole."""
        return [
            self._plant(self.published / "include" / "SkOld.h", "// old\n"),
            self._plant(self.published / "ios-gpu" / "lib" / "Release" / "libskia.a", "old ios\n"),
            self._plant(self.output_lib, "old mac\n"),
        ]

    def test_moved_branch_head_still_builds_the_exact_pin(self) -> None:
        self._manifest(self._builder(FAKE_BUILDER))
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.output_lib.read_text(encoding="utf-8").strip(), self.skia_pinned)
        self.assertNotEqual(self.skia_pinned, self.skia_head)
        invocations = (self.pulp / "external" / "skia-builder" / "invocations.log").read_text(
            encoding="utf-8"
        )
        self.assertIn(f"-revision {self.skia_pinned}", invocations)
        self.assertTrue((self.published / "include" / "SkFake.h").exists())
        self.assertTrue((self.published / "share" / "icudtl.dat").exists())

    def test_only_slices_built_in_this_run_are_published(self) -> None:
        builder_ref = self._builder(FAKE_BUILDER)
        self._manifest(builder_ref)
        # A persistent builder tree from an older revision: another platform's
        # slice, an extra arch under this platform, and a stale header.
        self._git("clone", "-q", (self.tmp / "builder").as_uri(),
                  str(self.pulp / "external" / "skia-builder"), cwd=self.tmp)
        self._plant(self.builder_out / "ios-gpu" / "lib" / "Release" / "libskia.a", "stale ios\n")
        self._plant(
            self.builder_out / "mac-gpu" / "lib" / "Release" / "x86_64" / "libskia.a", "stale arch\n"
        )
        self._plant(self.builder_out / "include" / "SkStale.h", "// stale\n")
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.output_lib.read_text(encoding="utf-8").strip(), self.skia_pinned)
        self.assertFalse((self.published / "ios-gpu").exists())
        self.assertFalse((self.published / "mac-gpu" / "lib" / "Release" / "x86_64").exists())
        self.assertFalse((self.published / "include" / "SkStale.h").exists())

    def test_publish_replaces_the_previous_generation_whole(self) -> None:
        self._manifest(self._builder(FAKE_BUILDER))
        old_header, old_ios, _ = self._previous_generation()
        tracked_header = self._plant(
            self.pulp / "external" / "skia-build" / "include" / "SkTracked.h", "// tracked\n"
        )
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.output_lib.read_text(encoding="utf-8").strip(), self.skia_pinned)
        self.assertFalse(old_header.exists())
        self.assertFalse(old_ios.exists())
        # Files outside the published build/ directory are not this script's.
        self.assertTrue(tracked_header.exists())
        leftovers = [p.name for p in (self.pulp / "external" / "skia-build").iterdir()
                     if p.name.startswith(".build.")]
        self.assertEqual(leftovers, [])

    @unittest.skipIf(hasattr(os, "geteuid") and os.geteuid() == 0, "root can read mode-0 files")
    def test_failed_copy_leaves_the_previous_generation_intact(self) -> None:
        self._manifest(self._builder(FAKE_BUILDER))
        previous = self._previous_generation()
        result = self._run(FAKE_UNREADABLE_OUTPUT="1")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        for path, content in zip(previous, ["// old\n", "old ios\n", "old mac\n"]):
            self.assertEqual(path.read_text(encoding="utf-8"), content)
        self.assertFalse((self.published / "include" / "SkFake.h").exists())
        self.assertFalse((self.published / "share").exists())
        leftovers = [p.name for p in (self.pulp / "external" / "skia-build").iterdir()
                     if p.name.startswith(".build.")]
        self.assertEqual(leftovers, [])

    def test_builder_that_builds_the_branch_head_fails_before_copy(self) -> None:
        self._manifest(self._builder(FAKE_BUILDER))
        result = self._run(FAKE_BUILDER_IGNORES_REVISION="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"built Skia commit {self.skia_head}", result.stderr)
        self.assertFalse((self.pulp / "external" / "skia-build").exists())

    def test_dawn_that_differs_from_the_manifest_fails_before_copy(self) -> None:
        self._manifest(self._builder(FAKE_BUILDER), dawn=self.dawn_newer)
        result = self._run()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"built Dawn commit {self.dawn_pinned}", result.stderr)
        self.assertFalse((self.pulp / "external" / "skia-build").exists())

    def test_builder_without_revision_support_fails_before_building(self) -> None:
        self._manifest(self._builder(LEGACY_BUILDER))
        result = self._run()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not support -revision", result.stderr)
        self.assertFalse((self.pulp / "external" / "skia-builder" / "invocations.log").exists())
        self.assertFalse((self.pulp / "external" / "skia-build").exists())

    def test_abbreviated_commit_override_fails_before_any_clone(self) -> None:
        self._manifest(self._builder(FAKE_BUILDER))
        result = self._run(SKIA_EXPECTED_COMMIT=self.skia_pinned[:12])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SKIA_EXPECTED_COMMIT must be a full 40-character", result.stderr)
        self.assertFalse((self.pulp / "external").exists())

    def test_checkout_owned_output_must_not_be_a_symlink(self) -> None:
        self._manifest(self._builder(FAKE_BUILDER))
        shared = self.tmp / "shared-cache"
        shared.mkdir()
        output = self.pulp / "external" / "skia-build"
        output.parent.mkdir(parents=True)
        output.symlink_to(shared, target_is_directory=True)
        result = self._run()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("refusing to publish source-built Skia through symlink", result.stderr)
        self.assertEqual(list(shared.iterdir()), [])

    def test_source_publish_removes_fetcher_provenance(self) -> None:
        self._manifest(self._builder(FAKE_BUILDER))
        output = self.pulp / "external" / "skia-build"
        output.mkdir(parents=True)
        for name in (".skia-asset-sha256", ".skia-generation-manifest.json", ".skia-source-archive.zip"):
            (output / name).write_text("stale\n", encoding="utf-8")
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in (".skia-asset-sha256", ".skia-generation-manifest.json", ".skia-source-archive.zip"):
            self.assertFalse((output / name).exists(), name)

    def test_dry_run_reports_manifest_pins_without_cloning(self) -> None:
        builder_ref = self._builder(FAKE_BUILDER)
        self._manifest(builder_ref)
        result = self._run(PULP_SKIA_DRY_RUN="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            f"skia-build-receipt platform=mac builder_ref={builder_ref} "
            f"skia_revision={self.skia_pinned} dawn={self.dawn_pinned}",
            result.stdout,
        )
        self.assertIn(
            f"python3 build-skia.py mac -branch {BRANCH} -revision {self.skia_pinned} --shallow",
            result.stdout,
        )
        self.assertFalse((self.pulp / "external").exists())


if __name__ == "__main__":
    unittest.main()
