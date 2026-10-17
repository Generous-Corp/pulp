#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import re
import stat
import tempfile
import tomllib
import unittest
from pathlib import Path

import generate

REPO = generate.ROOT


class GenerateTests(unittest.TestCase):
    def fixture(self, root: Path) -> None:
        (root / "tools/toolchain").mkdir(parents=True)
        (root / "tools/toolchain/manifest.toml").write_text(
            generate.SOURCE.read_text(encoding="utf-8"), encoding="utf-8"
        )
        (root / ".shipyard").mkdir()
        body = (
            "header\n[toolchain]\n"
            "# handwritten sentinel survives generation\n"
            "handwritten = true\n\n"
            "[brew]\npackages = [\n  \"old\",\n]\n\n"
            f"{generate.BEGIN}\nold\n{generate.END}\nfooter\n"
        )
        (root / ".shipyard/vm-image.toml").write_text(body, encoding="utf-8")
        (root / ".shipyard/vm-image.intel.toml").write_text(body, encoding="utf-8")
        for name in (
            generate.MISE_CONFIG, generate.MISE_TOOLS_CONFIG, generate.MISE_LOCK, generate.EXPECTED,
            generate.SHIPYARD_PIN, generate.SHIPYARD_SUMS,
        ):
            (root / name).write_bytes((REPO / name).read_bytes())

    def test_five_runs_are_byte_identical(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            generate.generate(root, None, False)
            first = (root / ".shipyard/vm-image.toml").read_bytes()
            for _ in range(4):
                generate.generate(root, None, False)
            self.assertEqual(first, (root / ".shipyard/vm-image.toml").read_bytes())
            generate.generate(root, None, True)

    def test_non_owned_keys_and_comments_survive(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            generate.generate(root, None, False)
            text = (root / ".shipyard/vm-image.intel.toml").read_text(encoding="utf-8")
            self.assertIn("# handwritten sentinel survives generation", text)
            self.assertIn("handwritten = true", text)
            self.assertNotIn('xcode = "26.5"', text)

    def test_platform_non_owned_fields_survive(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            tart = root / "tartci"
            (tart / "manifests").mkdir(parents=True)
            (tart / "manifests/pulp.macos.toml").write_text(
                f"[toolchain]\n\n[brew]\npackages = [\n  \"old\",\n]\n\n{generate.BEGIN}\nold\n{generate.END}\n",
                encoding="utf-8",
            )
            for platform, manager, extra in (("linux", "apt", "ninja = \"system\"\n"), ("windows", "choco", "msvc = \"14.44\"\nninja = \"system\"\n")):
                path = tart / "manifests" / f"pulp.{platform}.toml"
                path.write_text(
                    f"[toolchain]\n{extra}\n[{manager}]\npackages = [\n  \"old\",\n]\n\n{generate.BEGIN}\nold\n{generate.END}\n",
                    encoding="utf-8",
                )
            generate.generate(root, tart, False)
            linux = (tart / "manifests/pulp.linux.toml").read_text(encoding="utf-8")
            windows = (tart / "manifests/pulp.windows.toml").read_text(encoding="utf-8")
            self.assertIn('ninja = "system"', linux)
            self.assertIn('msvc = "14.44"', windows)
            self.assertNotIn("rust_targets", linux)
            self.assertNotIn("rust_targets", windows)

    def test_missing_authority_field_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "tools/toolchain").mkdir(parents=True)
            (root / "tools/toolchain/manifest.toml").write_text(
                'schema=1\n[toolchain]\nxcode="26.5"\n', encoding="utf-8"
            )
            with self.assertRaises(ValueError):
                generate.load(root / "tools/toolchain/manifest.toml")

    def test_planted_hand_edit_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            generate.generate(root, None, False)
            path = root / ".shipyard/vm-image.toml"
            path.write_text(
                path.read_text(encoding="utf-8").replace('cmake = "4.3.3"', 'cmake = "4.3.4"'),
                encoding="utf-8",
            )
            with self.assertRaises(ValueError):
                generate.generate(root, None, True)

    def test_source_digest_is_in_generated_block(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            generate.generate(root, None, False)
            digest = hashlib.sha256(
                (root / "tools/toolchain/manifest.toml").read_bytes()
            ).hexdigest()
            text = (root / ".shipyard/vm-image.toml").read_text(encoding="utf-8")
            self.assertIn(f"source_sha256 = {digest}", text)



def pinned_shipyard() -> dict:
    """The checkout's [shipyard] table, so these cases follow every pin bump."""
    return tomllib.loads(generate.SOURCE.read_text(encoding="utf-8"))["shipyard"]


# A tag newer than any real pin, for cases that bump the copy.
LATER_TAG = "v9.0.0"


class StrictPinTests(unittest.TestCase):
    """The manifest pins exact versions, and every generated file agrees."""

    def fixture(self, root: Path) -> None:
        GenerateTests.fixture(self, root)  # type: ignore[arg-type]
        generate.generate(root, None, False)

    def manifest(self, root: Path) -> Path:
        return root / "tools/toolchain/manifest.toml"

    def forge_lock(self, root: Path, text: str) -> dict:
        """A coordinated edit: change the lock AND record its new digest in the
        manifest, as someone hand-editing both files would. The structural
        checks must still refuse anything that is not a legitimate lock."""
        lock = root / generate.MISE_LOCK
        lock.write_text(text, encoding="utf-8")
        generate.record_lock_digest(self.manifest(root), hashlib.sha256(lock.read_bytes()).hexdigest())
        return generate.load(self.manifest(root))[0]

    def assert_lock_refuses(self, root: Path, text: str, needle: str) -> None:
        data = self.forge_lock(root, text)
        with self.assertRaises(ValueError) as ctx:
            generate.check_mise_lock(root / generate.MISE_LOCK, data)
        self.assertIn(needle, str(ctx.exception))

    def lock_text(self, root: Path) -> str:
        return (root / generate.MISE_LOCK).read_text(encoding="utf-8")

    def assert_check_refuses(self, root: Path, needle: str) -> None:
        with self.assertRaises(ValueError) as ctx:
            generate.generate(root, None, True)
        self.assertIn(needle, str(ctx.exception))

    def test_checked_in_files_match_the_manifest(self) -> None:
        # The drift test proper: the committed mise configs, the lock and the VM
        # image files are exactly what the committed manifest generates.
        generate.generate(REPO, None, True)

    def test_floating_selectors_are_refused(self) -> None:
        source = generate.SOURCE.read_text(encoding="utf-8")
        for key, floating in (("cmake", "homebrew"), ("rust", "stable"), ("python", "3.12"), ("tart", "latest")):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as td:
                path = Path(td) / "manifest.toml"
                path.write_text(
                    re.sub(rf'^{key} = "[^"]*"', f'{key} = "{floating}"', source, count=1, flags=re.M),
                    encoding="utf-8",
                )
                with self.assertRaises(ValueError) as ctx:
                    generate.load(path)
                self.assertIn("non-exact", str(ctx.exception))

    def test_every_pin_says_whether_it_is_observed_or_a_target(self) -> None:
        source = generate.SOURCE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "manifest.toml"
            path.write_text(re.sub(r"(?m)^node = \{ status = .*$", "", source), encoding="utf-8")
            with self.assertRaises(ValueError) as ctx:
                generate.load(path)
            self.assertIn("evidence.node", str(ctx.exception))

    def test_dependency_inventory_names_the_pinned_mise_release(self) -> None:
        # The weekly upstream-drift audit compares tools/deps/manifest.json's
        # mise tag with jdx/mise's latest release, so it must name the release
        # the toolchain manifest pins, or the audit measures a stale tag.
        version = tomllib.loads(generate.SOURCE.read_text(encoding="utf-8"))["mise"]["version"]
        inventory = json.loads((REPO / "tools/deps/manifest.json").read_text(encoding="utf-8"))
        rows = [d for d in inventory["dependencies"] if d["name"] == "mise"]
        self.assertEqual(1, len(rows))
        self.assertEqual(version, rows[0]["version"])
        self.assertEqual({"kind": "git-tag", "ref": f"v{version}"}, rows[0]["upstream"])

    def test_rust_is_not_a_mise_tool(self) -> None:
        source = generate.SOURCE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "manifest.toml"
            path.write_text(source.replace("[npm]", '[mise.tools.rust]\nbackend = "core:rust"\nurl_prefixes = ["https://static.rust-lang.org/"]\n\n[npm]', 1), encoding="utf-8")
            with self.assertRaises(ValueError) as ctx:
                generate.load(path)
            self.assertIn("rust must not be a [mise.tools] entry", str(ctx.exception))
        text = (REPO / generate.MISE_TOOLS_CONFIG).read_text(encoding="utf-8")
        self.assertNotRegex(text, r"(?m)^rust\b")

    def test_aqua_url_prefixes_must_name_the_backend_repository(self) -> None:
        source = generate.SOURCE.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "manifest.toml"
            path.write_text(
                source.replace('"https://github.com/openai/tart/releases/download/"', '"https://github.com/evil/tart/releases/download/"'),
                encoding="utf-8",
            )
            with self.assertRaises(ValueError) as ctx:
                generate.load(path)
            self.assertIn("is not the aqua:openai/tart repository", str(ctx.exception))

    def test_hand_edited_tools_config_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            path = root / generate.MISE_TOOLS_CONFIG
            path.write_text(path.read_text(encoding="utf-8").replace('ninja = "1.13.2"', 'ninja = "1.13.1"'), encoding="utf-8")
            self.assert_check_refuses(root, "stale or hand-edited")

    def test_hand_edited_expectation_refuses(self) -> None:
        # check_env reads expected.json, so an edit there would silently move
        # what every host is compared against.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            generate.generate(root, None, True)  # control: the copy is current
            path = root / generate.EXPECTED
            path.write_text(path.read_text(encoding="utf-8").replace('"cmake": "4.3.3"', '"cmake": "4.4.3"'), encoding="utf-8")
            self.assert_check_refuses(root, "stale or hand-edited")

    def test_expectation_carries_the_checked_pins(self) -> None:
        import json

        data, _ = generate.load()
        expected = json.loads((REPO / generate.EXPECTED).read_text(encoding="utf-8"))
        for key in generate.CHECKED:
            self.assertEqual(data["toolchain"][key], expected["tools"][key])
        self.assertEqual(data["mise"]["binary"]["sha256"], expected["mise"]["sha256"])
        self.assertEqual(data["mise"]["enforcement"], expected["enforcement"])

    def test_mise_binary_must_be_pinned_by_digest(self) -> None:
        source = generate.SOURCE.read_text(encoding="utf-8")
        digest = '25e38f796fff67fc5f4c44f6963d4b8ed15be82a5d9244f693cc55d40a245ee3'
        cases = (
            (f'macos-arm64 = "{digest}"', f'macos-arm64 = "{digest[:-1]}"', "64-hex"),
            (f'macos-arm64 = "{digest}"', f'macos-arm65 = "{digest}"', "64-hex"),
            ('path = "~/.local/opt/mise-2026.10.11/bin/mise"', 'path = "/opt/homebrew/bin/mise"', "with its version in the path"),
        )
        for old, new, needle in cases:
            with self.subTest(new=new), tempfile.TemporaryDirectory() as td:
                path = Path(td) / "manifest.toml"
                self.assertIn(old, source)
                path.write_text(source.replace(old, new, 1), encoding="utf-8")
                with self.assertRaises(ValueError) as ctx:
                    generate.load(path)
                self.assertIn(needle, str(ctx.exception))

    def test_a_pin_bump_that_edits_only_shipyard_toml_refuses(self) -> None:
        # `shipyard pin bump` rewrites tools/shipyard.toml alone; the manifest
        # stays the authority and the refusal names the command that moves it.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            generate.generate(root, None, True)  # control: the copy is current
            pin = root / generate.SHIPYARD_PIN
            current = pinned_shipyard()["version"]
            pin.write_text(pin.read_text(encoding="utf-8").replace(f'version = "{current}"', f'version = "{LATER_TAG}"'), encoding="utf-8")
            self.assert_check_refuses(root, "generate.py --shipyard <tag>")
            generate.generate(root, None, False)
            self.assertIn(f'version = "{current}"', pin.read_text(encoding="utf-8"))
            self.assertIn("# Pulp's pinned Shipyard release.", pin.read_text(encoding="utf-8"))

    def test_hand_edited_shipyard_digests_refuse(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            sums = root / generate.SHIPYARD_SUMS
            digest = pinned_shipyard()["assets"]["shipyard-linux-x64"]
            flipped = ("0" if digest[0] != "0" else "1") + digest[1:]
            text = sums.read_text(encoding="utf-8")
            self.assertIn(digest, text)
            sums.write_text(text.replace(digest, flipped), encoding="utf-8")
            self.assert_check_refuses(root, "stale or hand-edited")

    def test_shipyard_bump_takes_every_digest_from_the_release(self) -> None:
        assets = {"shipyard-linux-x64": "a" * 64, "shipyard-macos-arm64.dmg": "b" * 64}
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            seen = []
            original = generate.fetch_shipyard_assets, generate.fetch_shipyard_installer_sha256
            generate.fetch_shipyard_assets = lambda repo, tag: seen.append((repo, tag)) or dict(assets)
            generate.fetch_shipyard_installer_sha256 = lambda repo, tag: "d" * 64
            try:
                generate.generate(root, None, False, shipyard=LATER_TAG)
            finally:
                generate.fetch_shipyard_assets, generate.fetch_shipyard_installer_sha256 = original
            self.assertEqual([("danielraffel/Shipyard", LATER_TAG)], seen)
            data, _ = generate.load(root / "tools/toolchain/manifest.toml")
            self.assertEqual(LATER_TAG, data["shipyard"]["version"])
            self.assertEqual(assets, data["shipyard"]["assets"])  # old asset rows are gone
            self.assertIn(f'version = "{LATER_TAG}"', (root / generate.SHIPYARD_PIN).read_text(encoding="utf-8"))
            sums = (root / generate.SHIPYARD_SUMS).read_text(encoding="utf-8")
            self.assertIn("a" * 64 + "  shipyard-linux-x64", sums)
            self.assertIn("d" * 64 + "  install.sh", sums)
            self.assertEqual("d" * 64, data["shipyard"]["installer_sha256"])
            generate.generate(root, None, True)
            with self.assertRaises(ValueError):
                generate.record_shipyard_pin(root / "tools/toolchain/manifest.toml", "latest", assets, "d" * 64)
            with self.assertRaises(ValueError) as ctx:
                generate.generate(root, None, False, shipyard="v0.0.1")
            self.assertIn(f"older than the pinned {LATER_TAG}", str(ctx.exception))

    def test_shipyard_pin_must_be_exact(self) -> None:
        source = generate.SOURCE.read_text(encoding="utf-8")
        pin = pinned_shipyard()
        linux, installer = pin["assets"]["shipyard-linux-x64"], pin["installer_sha256"]
        cases = (
            (f'version = "{pin["version"]}"', 'version = "latest"', "exact release tag"),
            (f'"shipyard-linux-x64" = "{linux}"', f'"shipyard-linux-x64" = "{linux[:-1]}"', "64-hex"),
            ('repo = "danielraffel/Shipyard"', 'repo = "Shipyard"', "owner/name"),
            (f'installer_sha256 = "{installer}"', f'installer_sha256 = "{installer[:-1]}"', "sha256 of install.sh"),
        )
        for old, new, needle in cases:
            with self.subTest(new=new), tempfile.TemporaryDirectory() as td:
                self.assertIn(old, source)
                path = Path(td) / "manifest.toml"
                path.write_text(source.replace(old, new, 1), encoding="utf-8")
                with self.assertRaises(ValueError) as ctx:
                    generate.load(path)
                self.assertIn(needle, str(ctx.exception))

    def test_shipyard_is_never_a_build_input(self) -> None:
        # A wrong or missing Shipyard is a fleet finding, never a build one: it
        # is not a mise build tool, the build check does not read it, and no
        # configure or dependency bootstrap runs it.
        self.assertNotIn("shipyard", (REPO / generate.MISE_TOOLS_CONFIG).read_text(encoding="utf-8").lower())
        self.assertNotIn("shipyard", (REPO / generate.EXPECTED).read_text(encoding="utf-8").lower())
        self.assertNotIn("shipyard", (REPO / "tools/toolchain/check_env.py").read_text(encoding="utf-8").lower())
        invokes = re.compile(r"(^|[\s;&|(`$])shipyard\s+[a-z]")
        build_files = [REPO / "CMakeLists.txt", REPO / "setup.sh", REPO / "setup.ps1"]
        build_files += sorted((REPO / "tools/cmake").rglob("*.cmake"))
        self.assertGreater(len(build_files), 10)  # control: the scan reads real files
        for path in build_files:
            if not path.exists():
                continue
            for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                code = line.split("#", 1)[0]
                self.assertIsNone(invokes.search(code), f"{path}:{number} runs shipyard: {line.strip()}")

    def test_any_lock_edit_without_the_manifest_digest_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            path = root / generate.MISE_LOCK
            text = path.read_text(encoding="utf-8")
            digest = re.search(r'checksum = "sha256:([0-9a-f]{64})"', text).group(1)
            path.write_text(text.replace(digest, "0" * 64, 1), encoding="utf-8")
            self.assert_check_refuses(root, "but tools/toolchain/manifest.toml lock_sha256 is")

    def test_forged_lock_url_outside_the_backend_refuses(self) -> None:
        # The edit that survived the self-hash: a URL swap plus a recomputed digest.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            text = self.lock_text(root).replace(
                "https://github.com/openai/tart/releases/download/2.38.0/tart.tar.gz",
                "https://evil.example/tart.tar.gz", 1,
            )
            self.assert_lock_refuses(root, text, "https://evil.example/tart.tar.gz is outside its backend's repository")

    def test_forged_lock_url_in_another_repository_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            text = self.lock_text(root).replace(
                "https://github.com/ninja-build/ninja/releases/download/",
                "https://github.com/Kitware/CMake/releases/download/", 1,
            )
            self.assert_lock_refuses(root, text, "is outside its backend's repository")

    def test_forged_lock_url_for_another_version_refuses(self) -> None:
        # Inside the allowed prefix, but the wrong release.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            text = self.lock_text(root).replace(
                "https://github.com/openai/tart/releases/download/2.38.0/tart.tar.gz",
                "https://github.com/openai/tart/releases/download/2.37.0/tart.tar.gz", 1,
            )
            self.assert_lock_refuses(root, text, "is not version 2.38.0")
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            text = self.lock_text(root).replace("node-v26.10.0-", "node-v26.1.0-", 1)
            self.assert_lock_refuses(root, text, "is not version 26.10.0")

    def test_forged_conda_dependency_url_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            text = self.lock_text(root).replace("https://conda.anaconda.org/conda-forge/", "https://evil.example/conda/", 1)
            self.assert_lock_refuses(root, text, "conda-packages.")

    def test_forged_lock_version_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            text = self.lock_text(root).replace('version = "4.3.3"', 'version = "4.3.4"', 1)
            self.assert_lock_refuses(root, text, "cmake is locked at '4.3.4'")

    def test_lock_missing_a_platform_checksum_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            text = re.sub(
                r'(\[tools\.node\."platforms\.windows-arm64"\]\n)checksum = "[^"]*"\n', r"\1", self.lock_text(root)
            )
            self.assert_lock_refuses(root, text, "node@26.10.0 has no checksum and url for windows-arm64")

    def test_lock_missing_a_whole_platform_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            text = re.sub(r'\[tools\.cmake\."platforms\.linux-x64"\]\n(?:[a-z_]+ = .*\n)+', "", self.lock_text(root))
            self.assert_lock_refuses(root, text, "cmake@4.3.3 has no checksum and url for linux-x64")

    def test_lock_entry_on_a_declared_unlocked_platform_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            text = self.lock_text(root) + (
                '\n[tools.tart."platforms.linux-x64"]\nchecksum = "sha256:' + "1" * 64 + '"\n'
                'url = "https://github.com/openai/tart/releases/download/2.38.0/tart.tar.gz"\n'
            )
            self.assert_lock_refuses(root, text, "tart has entries for linux-x64")

    def test_lock_with_an_unmanifested_tool_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            text = self.lock_text(root) + '\n[[tools.jq]]\nversion = "1.8.1"\nbackend = "aqua:jqlang/jq"\n'
            self.assert_lock_refuses(root, text, "tools not in the manifest: jq")

    def test_conda_dependency_without_a_locked_package_refuses(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            text = re.sub(r'\[conda-packages\.linux-x64\."libllvm21[^"]*"\]\n(?:[a-z_]+ = .*\n)+', "", self.lock_text(root))
            self.assert_lock_refuses(root, text, "conda dependencies without a locked package on linux-x64")

    def test_every_lock_platform_is_locked_or_declared_unlocked(self) -> None:
        data, _ = generate.load()
        lock = tomllib.loads((REPO / generate.MISE_LOCK).read_text(encoding="utf-8"))
        for key, spec in data["mise"]["tools"].items():
            entry = lock["tools"][generate.mise_name(key, spec)][0]
            for platform in data["mise"]["lock_platforms"]:
                with self.subTest(tool=key, platform=platform):
                    row = entry.get(f"platforms.{platform}")
                    if platform in spec.get("unlocked", {}):
                        self.assertIsNone(row)
                    else:
                        self.assertTrue(row and row["checksum"].startswith("sha256:") and row["url"])

    def test_every_manifest_tool_is_in_the_generated_profile(self) -> None:
        data, _ = generate.load()
        text = (REPO / generate.MISE_TOOLS_CONFIG).read_text(encoding="utf-8")
        for key, spec in data["mise"]["tools"].items():
            name = generate.mise_name(key, spec)
            self.assertRegex(text, rf'(?m)^"?{re.escape(name)}"? = .*"{re.escape(data["toolchain"][key])}"')
        if data["mise"]["enforcement"] == "warn":
            # An older mise in an interactive shell must not print upgrade warnings.
            self.assertNotIn("min_version", text)
        base = (REPO / generate.MISE_CONFIG).read_text(encoding="utf-8")
        self.assertNotIn("[tools]", base)
        for prefix in data["npm"]["prefixes"]:
            self.assertTrue((REPO / prefix / "package-lock.json").is_file(), prefix)
            self.assertIn(f"npm ci --prefix {prefix}", base)

    def test_the_pinned_mise_is_asked_its_version_without_an_update_check(self) -> None:
        data, _ = generate.load()
        with tempfile.TemporaryDirectory() as td:
            mise = Path(td) / "mise"
            # Reports the pinned version only when the update check is off.
            mise.write_text(
                '#!/bin/sh\n[ "$MISE_DISABLE_UPDATE_WARNING" = 1 ] && echo "%s macos-arm64" || echo "0.0.0"\n'
                % data["mise"]["version"], encoding="utf-8")
            mise.chmod(0o755)
            generate.require_pinned_mise(mise, data)

    def mise_image(self, root: Path) -> Path:
        path = root / generate.MISE_IMAGE
        path.write_bytes((REPO / generate.MISE_IMAGE).read_bytes())
        return path

    def test_stale_mise_block_is_detected_and_repaired(self) -> None:
        # The golden layer reads its mise pin from this block, so a stale block
        # bakes the wrong binary. --check names it; a write restores it.
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            image = self.mise_image(root)
            good = image.read_text(encoding="utf-8")
            version = generate.load(self.manifest(root))[0]["mise"]["version"]
            stale = good.replace(f'version = "{version}"', 'version = "2020.1.1"', 1)
            self.assertNotEqual(good, stale)
            image.write_text(stale, encoding="utf-8")
            self.assert_check_refuses(root, "generated [mise] block differs")
            self.assertEqual(stale, image.read_text(encoding="utf-8"))
            generate.generate(root, None, False)
            self.assertEqual(good, image.read_text(encoding="utf-8"))
            generate.generate(root, None, True)

    def test_missing_mise_markers_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            image = self.mise_image(root)
            unmarked = (
                image.read_text(encoding="utf-8")
                .replace(generate.BEGIN + "\n", "")
                .replace(generate.END + "\n", "")
            )
            image.write_text(unmarked, encoding="utf-8")
            for check in (True, False):
                with self.assertRaises(ValueError) as ctx:
                    generate.generate(root, None, check)
                self.assertIn("generated toolchain block is missing", str(ctx.exception))
                self.assertEqual(unmarked, image.read_text(encoding="utf-8"))

    def test_lock_is_written_only_by_the_pinned_mise(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            fake = root / "mise"
            fake.write_text("#!/bin/sh\necho 2026.5.16 macos-arm64\n", encoding="utf-8")
            fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
            before = (root / generate.MISE_LOCK).read_bytes()
            with self.assertRaises(ValueError) as ctx:
                generate.generate(root, None, False, mise=fake)
            self.assertIn("must be written by mise", str(ctx.exception))
            self.assertEqual(before, (root / generate.MISE_LOCK).read_bytes())

    def test_lock_runs_never_ask_for_a_newer_mise(self) -> None:
        data, _ = generate.load()
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            self.fixture(root)
            seen = root / "seen-env"
            fake = root / "mise"
            fake.write_text(
                "#!/bin/sh\n"
                f'if [ "$1" = --version ]; then echo "{data["mise"]["version"]} macos-arm64"; exit 0; fi\n'
                f'echo "$1 MISE_DISABLE_UPDATE_WARNING=$MISE_DISABLE_UPDATE_WARNING" >> "{seen}"\n',
                encoding="utf-8")
            fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
            generate.write_mise_lock(root, data, fake)
            lines = seen.read_text(encoding="utf-8").splitlines()
            self.assertTrue(lines)  # control: the lock command ran
            self.assertTrue(all(line == "lock MISE_DISABLE_UPDATE_WARNING=1" for line in lines), lines)


if __name__ == "__main__":
    unittest.main()
