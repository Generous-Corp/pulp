#!/usr/bin/env python3
"""Deterministic Forge Modular-only release recipe and exclusion tests."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE_ROOT = Path(__file__).resolve().parents[2]


def run(*args: str, cwd: Path | None = None, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(args), cwd=cwd, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )


def commit_and_detach(root: Path, message: str) -> str:
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "add", "."], check=True)
    subprocess.run(
        ["git", "-C", str(root), "-c", "user.name=Fixture",
         "-c", "user.email=fixture@example.invalid", "commit", "-qm", message],
        check=True,
    )
    head = subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "HEAD"], text=True
    ).strip()
    subprocess.run(["git", "-C", str(root), "checkout", "-q", "--detach"], check=True)
    return head


class ModularReleasePackageTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.pulp = self.root / "pulp"
        self.forge = self.root / "forge"
        self.build = self.root / "build"
        self.expanded = self.root / "expanded"
        self.out = self.root / "out"
        self.capture = self.root / "recipe-args.json"
        self.arch_calls = self.root / "arch-calls.txt"
        self.version = "0.20.0"

        (self.pulp / "examples" / "forge-modular").mkdir(parents=True)
        (self.pulp / "tools" / "scripts").mkdir(parents=True)
        for name in ("release-package.sh", "release_inputs.py", "binary_identity.py"):
            source = SOURCE_ROOT / "examples" / "forge-modular" / name
            destination = self.pulp / "examples" / "forge-modular" / name
            shutil.copy2(source, destination)
            destination.chmod(0o755)

        recipe = self.pulp / "tools" / "scripts" / "build_combined_installer.sh"
        recipe.write_text(
            "#!/usr/bin/env python3\n"
            "import json, os, pathlib, sys\n"
            "args=sys.argv[1:]\n"
            "json.dump(args, open(os.environ['RECIPE_CAPTURE'], 'w'))\n"
            "out=pathlib.Path(args[args.index('--out')+1])\n"
            "version=args[args.index('--version')+1]\n"
            "out.mkdir(parents=True, exist_ok=True)\n"
            "(out / f'Forge Modular-{version}.pkg').write_bytes(b'fixture')\n"
        )
        recipe.chmod(0o755)
        arch = self.pulp / "tools" / "scripts" / "check_bundle_architectures.py"
        arch.write_text(
            "#!/bin/sh\n"
            "printf '%s\\n' \"$*\" >> \"$ARCH_CAPTURE\"\n"
        )
        arch.chmod(0o755)
        self.pulp_ref = commit_and_detach(self.pulp, "exact Pulp fixture")

        (self.forge / "tools").mkdir(parents=True)
        (self.forge / "PULP_SDK_REF").write_text(self.pulp_ref + "\n")
        validator = self.forge / "tools" / "validate-release-sdk.sh"
        validator.write_text("#!/bin/sh\nexit 0\n")
        validator.chmod(0o755)
        snapshot = self.forge / "tools" / "forge_source_snapshot.py"
        snapshot.write_text(
            "#!/usr/bin/env python3\n"
            "import os, subprocess, sys\n"
            "root=sys.argv[sys.argv.index('--root')+1]\n"
            "head=subprocess.check_output(['git','-C',root,'rev-parse','HEAD'], text=True).strip()\n"
            "print(f'source_git_head={head}')\n"
            "print('source_git_dirty=false')\n"
            "print('source_snapshot_sha256=' + os.environ.get('SNAPSHOT_CHAR', 'a') * 64)\n"
        )
        snapshot.chmod(0o755)
        self.forge_ref = commit_and_detach(self.forge, "exact Forge fixture")

        self._make_build(self.build)
        self._make_expanded(self.expanded)
        pkgutil = self.root / "pkgutil"
        pkgutil.write_text(
            "#!/bin/sh\n"
            "[ \"$1\" = --expand-full ] || exit 2\n"
            "cp -R \"$EXPANDED_FIXTURE\" \"$3\"\n"
        )
        pkgutil.chmod(0o755)
        self.pkgutil = pkgutil

    def tearDown(self) -> None:
        self.temp.cleanup()

    def _bundle(self, path: Path, kind: str) -> None:
        resources = path / "Contents" / "Resources"
        (resources / "tools" / "rack").mkdir(parents=True)
        (resources / "docs" / "status").mkdir(parents=True)
        (resources / "external" / "fonts").mkdir(parents=True)
        (resources / "examples" / "forge-modular").mkdir(parents=True)
        (resources / "core" / "signal" / "include").mkdir(parents=True)
        (resources / "build").mkdir(parents=True)
        snapshot = "a" * 64
        platform = "darwin-arm64"
        fields = {
            "schema": "1",
            "version": self.version,
            "packaged": "2026-08-30T00:00:00Z",
            "product": "Forge Modular",
            "product_id": "com.generous.forge.modular",
            "role": "Rack module and patch generator",
            "format": {
                "au": "Audio Unit", "vst3": "VST3", "clap": "CLAP",
                "standalone": "Standalone application",
            }[kind],
            "build": f"Release · {platform}",
            "pulp_sdk": f"0.823.2 · {self.pulp_ref}",
            "source_git_head": self.forge_ref,
            "source_git_dirty": "false",
            "source_snapshot_sha256": snapshot,
            "expected_pulp_sdk_ref": self.pulp_ref,
        }
        (resources / "FORGE_BUILD_INFO").write_text(
            "".join(f"{key}={value}\n" for key, value in fields.items())
        )
        (resources / "tools" / "rack" / "FORGE_TOOLCHAIN_STAMP").write_text(
            f"{self.version}\n2026-08-30\npulp {self.pulp_ref}\n"
        )
        (resources / "tools" / "rack" / "patch.py").write_text("# exact toolchain\n")
        decoder = resources / "tools" / "rack" / "rack_patch_decode"
        decoder.write_bytes(b"Mach-O decoder fixture")
        decoder.chmod(0o755)
        (resources / "tools" / "dsp_vocabulary.py").write_text("# vocabulary\n")
        (resources / "docs" / "status" / "agent-capabilities.json").write_text("{}\n")
        (resources / "external" / "fonts" / "Inter-Regular.ttf").write_bytes(b"font")
        (resources / "examples" / "forge-modular" / "plugin.json").write_text("{}\n")
        (resources / "core" / "signal" / "include" / "signal.hpp").write_text("// header\n")
        shape = resources / "build" / "shape_text"
        shape.write_bytes(b"Mach-O fixture")
        shape.chmod(0o755)

    def _make_build(self, build: Path) -> None:
        bundles = {
            "au": build / "AU" / "Forge Modular.component",
            "vst3": build / "VST3" / "Forge Modular.vst3",
            "clap": build / "CLAP" / "Forge Modular.clap",
            "standalone": build / "modular" / "Forge Modular.app",
        }
        for kind, path in bundles.items():
            self._bundle(path, kind)
        for sibling in (
            build / "AU" / "Forge FX.component",
            build / "VST3" / "Forge Instrument.vst3",
            build / "CLAP" / "Forge MIDI.clap",
            build / "sequencer" / "Forge Sequencer.app",
        ):
            sibling.mkdir(parents=True)
        (build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.forge}\n"
            "CMAKE_BUILD_TYPE:STRING=Release\n"
            "FORGE_TARGET_PLATFORM:INTERNAL=darwin-arm64\n"
            "PULP_SDK_PLATFORM:STRING=darwin-arm64\n"
            "FORGE_MODULAR_REQUIRE_TOOLCHAIN:BOOL=ON\n"
        )

    def _make_expanded(self, expanded: Path) -> None:
        payloads = {
            "au": expanded / "au.pkg" / "Payload" / "Library" / "Audio" / "Plug-Ins" / "Components" / "Forge Modular.component",
            "vst3": expanded / "vst3.pkg" / "Payload" / "Library" / "Audio" / "Plug-Ins" / "VST3" / "Forge Modular.vst3",
            "clap": expanded / "clap.pkg" / "Payload" / "Library" / "Audio" / "Plug-Ins" / "CLAP" / "Forge Modular.clap",
            "standalone": expanded / "app.pkg" / "Payload" / "Applications" / "Forge Modular.app",
        }
        for kind, path in payloads.items():
            self._bundle(path, kind)

    def _env(self) -> dict[str, str]:
        env = dict(os.environ)
        env.update({
            "HOME": str(self.root / "home"),
            "RECIPE_CAPTURE": str(self.capture),
            "ARCH_CAPTURE": str(self.arch_calls),
            "EXPANDED_FIXTURE": str(self.expanded),
            "FORGE_MODULAR_ARCH_CHECKER": str(
                self.pulp / "tools" / "scripts" / "check_bundle_architectures.py"
            ),
            "FORGE_MODULAR_PKGUTIL": str(self.pkgutil),
        })
        return env

    def _command(self) -> list[str]:
        return [
            "/bin/bash", str(self.pulp / "examples" / "forge-modular" / "release-package.sh"),
            "--forge-root", str(self.forge), "--forge-ref", self.forge_ref,
            "--build-dir", str(self.build), "--out", str(self.out),
            "--version", self.version, "--architecture", "arm64",
            "--sign-identity", "APPHASH", "--installer-identity", "INSTHASH",
        ]

    def test_recipe_contains_exactly_four_modular_payloads(self) -> None:
        completed = run(*self._command(), env=self._env())
        self.assertEqual(0, completed.returncode, completed.stdout + completed.stderr)
        args = json.loads(self.capture.read_text())
        self.assertEqual(3, args.count("--plugin"))
        self.assertEqual(1, args.count("--app-for"))
        joined = "\n".join(args)
        for name in ("Forge FX", "Forge Instrument", "Forge MIDI", "Forge Sequencer"):
            self.assertNotIn(name, joined)
        self.assertNotIn("--no-notarize", args)
        self.assertEqual(6, len(self.arch_calls.read_text().splitlines()))
        self.assertIn("verified signed/notarized Modular-only package", completed.stdout)

    def test_missing_format_fails_before_recipe(self) -> None:
        shutil.rmtree(self.build / "CLAP" / "Forge Modular.clap")
        completed = run(*self._command(), env=self._env())
        self.assertEqual(2, completed.returncode)
        self.assertIn("missing clap artifact", completed.stderr)
        self.assertFalse(self.capture.exists())

    def test_package_rejects_sibling_product_artifact(self) -> None:
        sibling = (
            self.expanded / "fx.pkg" / "Payload" / "Library" / "Audio" /
            "Plug-Ins" / "VST3" / "Forge FX.vst3"
        )
        sibling.mkdir(parents=True)
        helper = self.pulp / "examples" / "forge-modular" / "release_inputs.py"
        completed = run(
            str(helper), "package", "--expanded-root", str(self.expanded),
            "--forge-ref", self.forge_ref, "--pulp-ref", self.pulp_ref,
            "--version", self.version, "--architecture", "arm64",
            "--source-snapshot", "a" * 64,
        )
        self.assertEqual(2, completed.returncode)
        self.assertIn("non-Modular product artifacts", completed.stderr)

    def test_package_rejects_nested_sibling_product_artifact(self) -> None:
        sibling = (
            self.expanded / "app.pkg" / "Payload" / "Applications" /
            "Forge Modular.app" / "Contents" / "Resources" / "Forge FX.vst3"
        )
        sibling.mkdir(parents=True)
        helper = self.pulp / "examples" / "forge-modular" / "release_inputs.py"
        completed = run(
            str(helper), "package", "--expanded-root", str(self.expanded),
            "--forge-ref", self.forge_ref, "--pulp-ref", self.pulp_ref,
            "--version", self.version, "--architecture", "arm64",
            "--source-snapshot", "a" * 64,
        )
        self.assertEqual(2, completed.returncode)
        self.assertIn("non-Modular product artifacts", completed.stderr)

    def test_mismatched_toolchain_fails_before_recipe(self) -> None:
        patch = (
            self.build / "VST3" / "Forge Modular.vst3" / "Contents" /
            "Resources" / "tools" / "rack" / "patch.py"
        )
        patch.write_text("# stale toolchain\n")
        completed = run(*self._command(), env=self._env())
        self.assertEqual(2, completed.returncode)
        self.assertIn("do not share one exact bundled toolchain", completed.stderr)
        self.assertFalse(self.capture.exists())

    def test_stale_source_snapshot_fails_before_recipe(self) -> None:
        env = self._env()
        env["SNAPSHOT_CHAR"] = "b"
        completed = run(*self._command(), env=env)
        self.assertEqual(2, completed.returncode)
        self.assertIn("do not match the canonical Forge source snapshot", completed.stderr)
        self.assertFalse(self.capture.exists())

    def test_missing_rack_decoder_fails_before_recipe(self) -> None:
        decoder = (
            self.build / "AU" / "Forge Modular.component" / "Contents" /
            "Resources" / "tools" / "rack" / "rack_patch_decode"
        )
        decoder.unlink()
        completed = run(*self._command(), env=self._env())
        self.assertEqual(2, completed.returncode)
        self.assertIn("Rack saved-patch decoder is not executable", completed.stderr)
        self.assertFalse(self.capture.exists())


if __name__ == "__main__":
    unittest.main()
