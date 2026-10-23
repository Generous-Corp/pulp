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

base = pathlib.Path(__file__).resolve().parent
with (base / "invocations.log").open("a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")


def git(*command, cwd=None):
    return subprocess.run(
        ["git", *command], cwd=cwd, check=True, capture_output=True, text=True
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
git("checkout", "--detach", (src / "DAWN_PIN").read_text().strip(), cwd=dawn)

out = base / "build"
(out / "include").mkdir(parents=True, exist_ok=True)
(out / "include" / "SkFake.h").write_text("// fake\n")
lib = out / args.platform / "lib"
lib.mkdir(parents=True, exist_ok=True)
(lib / "libskia.a").write_text(git("rev-parse", "HEAD", cwd=src) + "\n")
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
(pathlib.Path(__file__).resolve().parent / "invocations.log").write_text("ran\n")
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
            ["git", *command], cwd=cwd, env=self.env, check=True, capture_output=True, text=True
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
            capture_output=True,
            check=False,
        )

    @property
    def output_lib(self) -> Path:
        return self.pulp / "external" / "skia-build" / "mac" / "lib" / "libskia.a"

    def test_moved_branch_head_still_builds_the_exact_pin(self) -> None:
        self._manifest(self._builder(FAKE_BUILDER))
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.output_lib.read_text().strip(), self.skia_pinned)
        self.assertNotEqual(self.skia_pinned, self.skia_head)
        invocations = (self.pulp / "external" / "skia-builder" / "invocations.log").read_text()
        self.assertIn(f"-revision {self.skia_pinned}", invocations)
        self.assertTrue((self.pulp / "external" / "skia-build" / "include" / "SkFake.h").exists())

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
