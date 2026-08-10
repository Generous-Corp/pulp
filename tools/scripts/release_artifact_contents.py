#!/usr/bin/env python3
"""Verify the contents of Pulp release archives against the product matrix.

The outer release-asset check proves that files with the expected names exist.
This verifier proves that those files contain the products users are entitled to
receive.  It deliberately runs twice in release-cli.yml: once on each native
build runner (where macOS can perform a real code-signature verification), and
again on the assets downloaded from the GitHub release draft immediately before
publication.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from sdk_capability_handoff import (
    CAPABILITIES_PATH,
    CAPABILITIES_SCHEMA_PATH,
    HANDOFF_PATH,
    HANDOFF_SCHEMA_PATH,
    HandoffError,
    importer_path,
    verify_payload as verify_capability_handoff_payload,
)


DEFAULT_MATRIX_PATH = Path(__file__).with_name("release_product_matrix.json")
LEGACY_CLI_BINARY_STEMS = frozenset({"pulp", "pulp-cpp", "pulp-mcp"})
IMPORT_DESIGN_CLI_FLOOR = "0.764.0"
PRE_DECLARATIVE_IMPORT_DESIGN_CLI_BINARY_STEMS = LEGACY_CLI_BINARY_STEMS | {
    "pulp-import-design"
}
PRE_DECLARATIVE_IMPORT_DESIGN_COMMON_CLI_MEMBERS = frozenset(
    {
        "browser_capture/browser_process.mjs",
        "browser_capture/capture.mjs",
        "browser_capture/health.mjs",
        "browser_capture/lifecycle.mjs",
        "browser_capture/network_dependencies.mjs",
        "browser_capture/renderers.mjs",
        "browser_capture/security.mjs",
        "browser_capture/semantics.mjs",
        "browser_capture/settle.mjs",
        "browser_capture/tokens.mjs",
    }
)
CONTROL_BROKER_CLI_MEMBER = "pulp-control-broker"
CONTROL_BROKER_SDK_MEMBER = "pulp-sdk/libexec/pulp/pulp-control-broker"


class ContentError(RuntimeError):
    pass


def version_tuple(value: str) -> tuple[int, int, int]:
    parts = value.removeprefix("v").split(".")
    if len(parts) != 3 or any(not part.isdigit() for part in parts):
        raise ContentError(f"invalid release version: {value!r}")
    return int(parts[0]), int(parts[1]), int(parts[2])


def control_broker_required(
    platform: str, matrix: ProductMatrix, version: str | None
) -> bool:
    if not platform.startswith("darwin-"):
        return False
    if version is None:
        return matrix.control_broker_floor != "999999.0.0"
    return version_tuple(version) >= version_tuple(matrix.control_broker_floor)


@dataclass(frozen=True)
class ProductMatrix:
    contract_floor: str
    sdk_provenance_floor: str
    capability_handoff_floor: str
    inspector_sdk_floor: str
    control_broker_floor: str
    platforms: tuple[str, ...]
    cli_contract_declared: bool
    cli_binary_stems: frozenset[str]
    common_cli_members: frozenset[str]
    pulp_library_stems: frozenset[str]
    platform_library_stems: dict[str, frozenset[str]]
    common_sdk_members: frozenset[str]
    darwin_sdk_members: frozenset[str]

    @classmethod
    def load(cls, path: Path) -> ProductMatrix:
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
            cli_contract_declared = (
                "cli_binary_stems" in doc and "common_cli_members" in doc
            )
            if ("cli_binary_stems" in doc) != ("common_cli_members" in doc):
                raise ContentError(
                    f"invalid release product matrix {path}: "
                    "CLI contract fields must be declared together"
                )
            matrix = cls(
                contract_floor=str(doc["contract_floor"]),
                # Historical product matrices predate positive SDK provenance.
                # An absent key therefore means "no release covered by this
                # historical matrix requires the marker", not malformed data.
                sdk_provenance_floor=str(
                    doc.get("sdk_provenance_floor", "999999.0.0")
                ),
                capability_handoff_floor=str(
                    doc.get("capability_handoff_floor", "999999.0.0")
                ),
                inspector_sdk_floor=str(
                    doc.get("inspector_sdk_floor", "999999.0.0")
                ),
                control_broker_floor=str(
                    doc.get("control_broker_floor", "999999.0.0")
                ),
                platforms=tuple(doc["platforms"]),
                cli_contract_declared=cli_contract_declared,
                # Matrices versioned before the import-design CLI payload did
                # not declare CLI members. Their release version selects the
                # exact historical contract during a backfill.
                cli_binary_stems=frozenset(
                    doc.get("cli_binary_stems", LEGACY_CLI_BINARY_STEMS)
                ),
                common_cli_members=frozenset(doc.get("common_cli_members", ())),
                pulp_library_stems=frozenset(doc["pulp_library_stems"]),
                platform_library_stems={
                    key: frozenset(value)
                    for key, value in doc["platform_library_stems"].items()
                },
                common_sdk_members=frozenset(doc["common_sdk_members"]),
                darwin_sdk_members=frozenset(doc["darwin_sdk_members"]),
            )
        except (KeyError, TypeError, json.JSONDecodeError, OSError) as exc:
            raise ContentError(f"invalid release product matrix {path}: {exc}") from exc
        if (
            not matrix.platforms
            or not matrix.cli_binary_stems
            or not matrix.pulp_library_stems
        ):
            raise ContentError(f"invalid release product matrix {path}: empty contract")
        version_tuple(matrix.contract_floor)
        version_tuple(matrix.sdk_provenance_floor)
        version_tuple(matrix.capability_handoff_floor)
        version_tuple(matrix.inspector_sdk_floor)
        version_tuple(matrix.control_broker_floor)
        return matrix


DEFAULT_MATRIX = ProductMatrix.load(DEFAULT_MATRIX_PATH)
PLATFORMS = DEFAULT_MATRIX.platforms


@dataclass(frozen=True)
class Member:
    name: str
    raw_name: str
    mode: int


class Archive:
    """Format-neutral, path-safe archive reader.

    Windows SDK artifacts retain the historical ``.tar.gz`` name even though
    PowerShell produces ZIP data, so detection must use bytes rather than the
    suffix.
    """

    def __init__(self, path: Path):
        self.path = path
        self._zip: zipfile.ZipFile | None = None
        self._tar: tarfile.TarFile | None = None
        if zipfile.is_zipfile(path):
            self._zip = zipfile.ZipFile(path)
            infos = self._zip.infolist()
            for info in infos:
                self._safe_name(info.filename)
            raw = [
                Member(info.filename, info.filename, (info.external_attr >> 16) & 0o777)
                for info in infos
                if not info.is_dir()
            ]
        else:
            try:
                self._tar = tarfile.open(path, "r:*")
            except (tarfile.TarError, OSError) as exc:
                raise ContentError(f"{path.name}: not a readable tar or zip archive") from exc
            infos = self._tar.getmembers()
            for info in infos:
                self._safe_name(info.name)
            raw = [
                Member(info.name, info.name, info.mode & 0o777)
                for info in infos
                if info.isfile()
            ]

        members: dict[str, Member] = {}
        for member in raw:
            name = self._safe_name(member.name)
            if name in members:
                raise ContentError(f"{path.name}: duplicate archive member {name!r}")
            members[name] = Member(name, member.raw_name, member.mode)
        self.members = members

    @property
    def preserves_modes(self) -> bool:
        return self._tar is not None

    @staticmethod
    def _safe_name(raw: str) -> str:
        normalized = raw.replace("\\", "/").removeprefix("./")
        path = PurePosixPath(normalized)
        if not normalized or path.is_absolute() or ".." in path.parts:
            raise ContentError(f"unsafe archive member path: {raw!r}")
        return path.as_posix()

    def read(self, name: str, *, limit: int = 1024 * 1024) -> bytes:
        if name not in self.members:
            raise ContentError(f"{self.path.name}: missing member {name}")
        raw_name = self.members[name].raw_name
        if self._zip is not None:
            with self._zip.open(raw_name) as handle:
                data = handle.read(limit + 1)
        else:
            assert self._tar is not None
            info = self._tar.getmember(raw_name)
            handle = self._tar.extractfile(info)
            if handle is None:
                raise ContentError(f"{self.path.name}: cannot read member {name}")
            with handle:
                data = handle.read(limit + 1)
        if len(data) > limit:
            raise ContentError(f"{self.path.name}: {name} exceeds read limit")
        return data

    def close(self) -> None:
        if self._zip is not None:
            self._zip.close()
        if self._tar is not None:
            self._tar.close()

    def __enter__(self) -> Archive:
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()


def cli_asset_name(platform: str) -> str:
    suffix = ".zip" if platform.startswith("windows-") else ".tar.gz"
    return f"pulp-{platform}{suffix}"


def sdk_asset_name(platform: str) -> str:
    return f"pulp-sdk-{platform}.tar.gz"


def cli_binary_members(
    platform: str,
    matrix: ProductMatrix = DEFAULT_MATRIX,
    version: str | None = None,
) -> frozenset[str]:
    suffix = ".exe" if platform.startswith("windows-") else ""
    stems, _resources = effective_cli_contract(matrix, version)
    members = {f"{stem}{suffix}" for stem in stems}
    if control_broker_required(platform, matrix, version):
        members.add(CONTROL_BROKER_CLI_MEMBER)
    return frozenset(members)


def effective_cli_contract(
    matrix: ProductMatrix, version: str | None
) -> tuple[frozenset[str], frozenset[str]]:
    # The declarative matrix follows current main, while backfills validate the
    # payload that the requested tag could actually package.
    if version is not None:
        requested = version_tuple(version)
        import_design_floor = version_tuple(IMPORT_DESIGN_CLI_FLOOR)
        if requested < import_design_floor:
            return LEGACY_CLI_BINARY_STEMS, frozenset()
        if requested == import_design_floor:
            return (
                frozenset(PRE_DECLARATIVE_IMPORT_DESIGN_CLI_BINARY_STEMS),
                PRE_DECLARATIVE_IMPORT_DESIGN_COMMON_CLI_MEMBERS,
            )
    if matrix.cli_contract_declared:
        return matrix.cli_binary_stems, matrix.common_cli_members
    if version is not None:
        return (
            frozenset(PRE_DECLARATIVE_IMPORT_DESIGN_CLI_BINARY_STEMS),
            PRE_DECLARATIVE_IMPORT_DESIGN_COMMON_CLI_MEMBERS,
        )
    return LEGACY_CLI_BINARY_STEMS, frozenset()


def cli_runtime_member(platform: str) -> str:
    if platform.startswith("windows-"):
        return "wgpu_native.dll"
    return (
        "libwgpu_native.dylib"
        if platform.startswith("darwin-")
        else "libwgpu_native.so"
    )


def cli_members(
    platform: str,
    matrix: ProductMatrix = DEFAULT_MATRIX,
    version: str | None = None,
) -> frozenset[str]:
    _binaries, resources = effective_cli_contract(matrix, version)
    return (
        cli_binary_members(platform, matrix, version)
        | resources
        | frozenset({cli_runtime_member(platform)})
    )


def sdk_import_design_runtime_members(
    matrix: ProductMatrix = DEFAULT_MATRIX,
    version: str | None = None,
) -> frozenset[str]:
    _binaries, resources = effective_cli_contract(matrix, version)
    prefix = "browser_capture/"
    return frozenset(
        f"pulp-sdk/bin/browser_capture-v1/{member.removeprefix(prefix)}"
        for member in resources
        if member.startswith(prefix)
    )


def sdk_binary_members(
    platform: str,
    matrix: ProductMatrix = DEFAULT_MATRIX,
    version: str | None = None,
) -> frozenset[str]:
    suffix = ".exe" if platform.startswith("windows-") else ""
    names = {f"pulp-sdk/bin/{name}{suffix}" for name in ("pulp", "pulp-cpp", "pulp-mcp", "pulp-scan-worker")}
    if platform.startswith("darwin-"):
        names.update(
            {
                "pulp-sdk/bin/pulp-au-effect-ab",
                "pulp-sdk/bin/pulp-au-instrument-probe",
            }
        )
        if control_broker_required(platform, matrix, version):
            names.add(CONTROL_BROKER_SDK_MEMBER)
    return frozenset(names)


def sdk_library_member(stem: str, platform: str) -> str:
    if platform.startswith("windows-"):
        return f"pulp-sdk/lib/{stem}.lib"
    return f"pulp-sdk/lib/lib{stem}.a"


def platform_family(platform: str) -> str:
    return platform.split("-", 1)[0]


def expected_pulp_libraries(platform: str, matrix: ProductMatrix) -> frozenset[str]:
    expected = set(matrix.pulp_library_stems)
    expected.update(matrix.platform_library_stems.get(platform_family(platform), ()))
    return frozenset(expected)


def required_sdk_members(
    platform: str,
    matrix: ProductMatrix = DEFAULT_MATRIX,
    version: str | None = None,
) -> frozenset[str]:
    required = set(matrix.common_sdk_members)
    required.update(sdk_binary_members(platform, matrix, version))
    required.update(
        sdk_library_member(stem, platform)
        for stem in expected_pulp_libraries(platform, matrix)
    )
    required.add(
        "pulp-sdk/lib/wgpu_native.dll"
        if platform.startswith("windows-")
        else (
            "pulp-sdk/lib/libwgpu_native.dylib"
            if platform.startswith("darwin-")
            else "pulp-sdk/lib/libwgpu_native.so"
        )
    )
    required.add(
        "pulp-sdk/lib/vst3-sdk.lib"
        if platform.startswith("windows-")
        else "pulp-sdk/lib/libvst3-sdk.a"
    )
    if platform.startswith("darwin-"):
        required.update(matrix.darwin_sdk_members)
    if (
        version is not None
        and version_tuple(version) >= version_tuple(matrix.sdk_provenance_floor)
    ):
        required.add("pulp-sdk/sdk-provenance.json")
    if (
        version is not None
        and version_tuple(version) >= version_tuple(matrix.capability_handoff_floor)
    ):
        required.update(
            {
                f"pulp-sdk/{HANDOFF_PATH.as_posix()}",
                f"pulp-sdk/{HANDOFF_SCHEMA_PATH.as_posix()}",
                f"pulp-sdk/{CAPABILITIES_PATH.as_posix()}",
                f"pulp-sdk/{CAPABILITIES_SCHEMA_PATH.as_posix()}",
                f"pulp-sdk/{importer_path(platform).as_posix()}",
            }
        )
        required.update(sdk_import_design_runtime_members(matrix, version))
    return frozenset(required)


def installed_pulp_libraries(names: set[str], platform: str) -> set[str]:
    prefix = "pulp-sdk/lib/"
    result: set[str] = set()
    for name in names:
        if not name.startswith(prefix):
            continue
        leaf = name.removeprefix(prefix)
        if "/" in leaf:
            continue
        if platform.startswith("windows-"):
            if leaf.startswith("pulp-") and leaf.endswith(".lib"):
                result.add(leaf[:-4])
        elif leaf.startswith("libpulp-") and leaf.endswith(".a"):
            result.add(leaf[len("lib") : -2])
    return result


def require_executable(archive: Archive, names: frozenset[str]) -> None:
    # ZIPs created by Compress-Archive do not retain POSIX modes; Windows has no
    # executable bit contract. Unix tarballs do, and installers rely on it.
    if not archive.preserves_modes:
        return
    bad = sorted(name for name in names if archive.members[name].mode & 0o111 == 0)
    if bad:
        raise ContentError(f"{archive.path.name}: non-executable shipped binary(s): {bad}")


def verify_cli_archive(
    path: Path,
    platform: str,
    version: str,
    matrix: ProductMatrix = DEFAULT_MATRIX,
) -> None:
    with Archive(path) as archive:
        expected = cli_members(platform, matrix, version)
        actual = set(archive.members)
        missing = sorted(expected - actual)
        unexpected = sorted(actual - expected)
        if missing or unexpected:
            raise ContentError(
                f"{path.name}: CLI product matrix mismatch; missing={missing}, "
                f"unexpected={unexpected}"
            )
        if not platform.startswith("windows-"):
            require_executable(
                archive, cli_binary_members(platform, matrix, version)
            )


def verify_sdk_archive(
    path: Path,
    platform: str,
    version: str,
    source_sha: str,
    matrix: ProductMatrix = DEFAULT_MATRIX,
) -> None:
    with Archive(path) as archive:
        names = set(archive.members)
        required = required_sdk_members(platform, matrix, version)
        missing = sorted(required - names)
        if missing:
            raise ContentError(f"{path.name}: missing SDK product(s): {missing}")
        if (
            CONTROL_BROKER_SDK_MEMBER in names
            and not control_broker_required(platform, matrix, version)
        ):
            raise ContentError(
                f"{path.name}: stale pre-floor SDK product: "
                f"{CONTROL_BROKER_SDK_MEMBER}"
            )

        actual_libraries = installed_pulp_libraries(names, platform)
        expected_libraries = expected_pulp_libraries(platform, matrix)
        missing_libraries = sorted(expected_libraries - actual_libraries)
        stale_libraries = sorted(actual_libraries - expected_libraries)
        if missing_libraries or stale_libraries:
            raise ContentError(
                f"{path.name}: Pulp library target matrix mismatch; "
                f"missing={missing_libraries}, stale_or_unexpected={stale_libraries}"
            )

        build_type = archive.read("pulp-sdk/sdk_build_type.txt").decode("utf-8").strip()
        if build_type != "Release":
            raise ContentError(f"{path.name}: sdk_build_type.txt is {build_type!r}, expected 'Release'")
        archived_version = archive.read("pulp-sdk/version.txt").decode("utf-8").strip()
        if archived_version != version:
            raise ContentError(
                f"{path.name}: version.txt is {archived_version!r}, expected {version!r}"
            )
        if version_tuple(version) >= version_tuple(matrix.sdk_provenance_floor):
            provenance_member = "pulp-sdk/sdk-provenance.json"
            if (
                archive.preserves_modes
                and archive.members[provenance_member].mode & 0o004 == 0
            ):
                raise ContentError(
                    f"{path.name}: {provenance_member} is not world-readable"
                )
            try:
                provenance = json.loads(
                    archive.read(provenance_member).decode("utf-8")
                )
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                raise ContentError(
                    f"{path.name}: sdk-provenance.json is invalid: {exc}"
                ) from exc
            expected = {
                "schema": "pulp.sdk-provenance.v1",
                "kind": "release",
                "profile": "official-release",
                "distribution_eligible": True,
                "sdk_version": version,
                "source_git_ref": f"v{version}",
                "source_git_sha": source_sha,
                "source_git_dirty": False,
                "platform": platform,
                "build_type": "Release",
                "features": {
                    "audio_probes": False,
                    "inspector": version_tuple(version)
                    >= version_tuple(matrix.inspector_sdk_floor),
                },
            }
            mismatches = {
                key: (provenance.get(key), value)
                for key, value in expected.items()
                if provenance.get(key) != value
            }
            marker_source_sha = provenance.get("source_git_sha")
            if not isinstance(marker_source_sha, str) or not re.fullmatch(
                r"[0-9a-f]{40}", marker_source_sha
            ):
                mismatches["source_git_sha"] = (
                    marker_source_sha,
                    "40 lowercase hex characters",
                )
            if mismatches:
                raise ContentError(
                    f"{path.name}: unsafe SDK provenance contract: {mismatches}"
                )
        if version_tuple(version) >= version_tuple(matrix.capability_handoff_floor):
            prefix = "pulp-sdk/"
            expected_runtime = sdk_import_design_runtime_members(matrix, version)
            runtime_prefix = "pulp-sdk/bin/browser_capture-v1/"
            actual_runtime = {
                name for name in names if name.startswith(runtime_prefix)
            }
            if actual_runtime != expected_runtime:
                raise ContentError(
                    f"{path.name}: importer runtime matrix mismatch; "
                    f"missing={sorted(expected_runtime - actual_runtime)}, "
                    f"stale_or_unexpected={sorted(actual_runtime - expected_runtime)}"
                )
            try:
                verify_capability_handoff_payload(
                    handoff_bytes=archive.read(
                        prefix + HANDOFF_PATH.as_posix(), limit=16 * 1024 * 1024
                    ),
                    handoff_schema_bytes=archive.read(
                        prefix + HANDOFF_SCHEMA_PATH.as_posix()
                    ),
                    importer_bytes=archive.read(
                        prefix + importer_path(platform).as_posix(),
                        limit=512 * 1024 * 1024,
                    ),
                    importer_runtime_bytes={
                        member.removeprefix(prefix): archive.read(member)
                        for member in expected_runtime
                    },
                    capability_bytes=archive.read(
                        prefix + CAPABILITIES_PATH.as_posix(),
                        limit=16 * 1024 * 1024,
                    ),
                    capability_schema_bytes=archive.read(
                        prefix + CAPABILITIES_SCHEMA_PATH.as_posix(),
                        limit=16 * 1024 * 1024,
                    ),
                    expected_sdk_source_sha=source_sha,
                    expected_platform=platform,
                )
            except HandoffError as exc:
                raise ContentError(
                    f"{path.name}: invalid agent capability handoff: {exc}"
                ) from exc
        if not platform.startswith("windows-"):
            require_executable(archive, sdk_binary_members(platform, matrix, version))

        if not platform.startswith("darwin-"):
            apple_only = sorted(
                name
                for name in names
                if name == "pulp-sdk/lib/libausdk.a"
                or name == "pulp-sdk/lib/ausdk.lib"
                or name.startswith("pulp-sdk/external/AudioUnitSDK/")
            )
            if apple_only:
                raise ContentError(f"{path.name}: stale Apple-only SDK products: {apple_only[:10]}")


def verify_native_macos_signatures(
    asset_dir: Path,
    platform: str,
    version: str,
    matrix: ProductMatrix = DEFAULT_MATRIX,
) -> None:
    if not platform.startswith("darwin-"):
        return
    if sys.platform != "darwin":
        raise ContentError("--native-signatures for Darwin archives requires a macOS runner")

    targets = {
        cli_asset_name(platform): cli_binary_members(platform, matrix, version)
        | frozenset({cli_runtime_member(platform)}),
        sdk_asset_name(platform): sdk_binary_members(platform, matrix, version)
        | frozenset({"pulp-sdk/lib/libwgpu_native.dylib"}),
    }
    with tempfile.TemporaryDirectory(prefix="pulp-release-signatures-") as td:
        root = Path(td)
        for asset_name, members in targets.items():
            with Archive(asset_dir / asset_name) as archive:
                for member in sorted(members):
                    data = archive.read(member, limit=256 * 1024 * 1024)
                    target = root / asset_name / member
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(data)
                    target.chmod(0o755)
                    result = subprocess.run(
                        ["codesign", "--verify", "--strict", "--verbose=2", str(target)],
                        capture_output=True,
                        text=True,
                    )
                    if result.returncode != 0:
                        detail = (result.stderr or result.stdout).strip()
                        raise ContentError(
                            f"{asset_name}:{member}: invalid or missing code signature: {detail}"
                        )


def verify_platform(
    asset_dir: Path,
    platform: str,
    version: str,
    source_sha: str,
    *,
    native_signatures: bool,
    matrix: ProductMatrix = DEFAULT_MATRIX,
) -> None:
    if platform not in matrix.platforms:
        raise ContentError(f"unsupported release platform: {platform}")
    cli = asset_dir / cli_asset_name(platform)
    sdk = asset_dir / sdk_asset_name(platform)
    for path in (cli, sdk):
        if not path.is_file():
            raise ContentError(f"missing release archive: {path.name}")
    verify_cli_archive(cli, platform, version, matrix)
    verify_sdk_archive(sdk, platform, version, source_sha, matrix)
    if native_signatures:
        verify_native_macos_signatures(asset_dir, platform, version, matrix)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("asset_dir", type=Path)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--platform", choices=PLATFORMS)
    group.add_argument("--all-platforms", action="store_true")
    parser.add_argument("--version", required=True, help="Release version without the leading v")
    parser.add_argument(
        "--source-sha", required=True, help="Exact 40-character release-tag commit"
    )
    parser.add_argument("--matrix", type=Path, default=DEFAULT_MATRIX_PATH)
    parser.add_argument("--native-signatures", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        matrix = ProductMatrix.load(args.matrix)
        platforms = matrix.platforms if args.all_platforms else (args.platform,)
        if version_tuple(args.version) < version_tuple(matrix.contract_floor):
            print(
                f"SKIP: {args.version} predates release-content contract "
                f"{matrix.contract_floor}"
            )
            return 0
        for platform in platforms:
            assert platform is not None
            verify_platform(
                args.asset_dir,
                platform,
                args.version,
                args.source_sha,
                native_signatures=args.native_signatures,
                matrix=matrix,
            )
            print(f"OK: {platform} release archives match the product matrix")
    except (ContentError, OSError, UnicodeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
