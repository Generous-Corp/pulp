#!/usr/bin/env python3
"""Install the canonical Linux packages needed to compile native Pulp targets."""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
from collections.abc import Callable, Iterator, Sequence

MANIFEST = pathlib.Path(__file__).with_name("linux_build_deps.json")
PACKAGE_RE = re.compile(r"^[a-z0-9][a-z0-9+.-]*$")
MAX_ATTEMPTS = 3
UNRELATED_THIRD_PARTY_APT_HOSTS = frozenset({"packages.microsoft.com"})
APT_SOURCE_SUFFIXES = frozenset({".list", ".sources"})
URL_RE = re.compile(r"https?://[^\s'\"]+")
ONE_LINE_SOURCE_RE = re.compile(
    r"^\s*deb(?:-src)?\s+(?:\[[^\]]*\]\s+)?(?P<uri>\S+)",
    re.IGNORECASE,
)


def load_profiles(path: pathlib.Path = MANIFEST) -> dict[str, list[str]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema_version") != 1:
        raise ValueError(f"{path}: unsupported schema_version")
    profiles = data.get("profiles")
    if not isinstance(profiles, dict) or not profiles:
        raise ValueError(f"{path}: profiles must be a non-empty object")

    definitions: dict[str, tuple[list[str], list[str]]] = {}
    for name, definition in profiles.items():
        if not isinstance(name, str) or not name:
            raise ValueError(f"{path}: profile names must be non-empty strings")
        if not isinstance(definition, dict):
            raise TypeError(f"{path}: profile {name!r} must be an object")
        unknown_keys = set(definition) - {"extends", "packages"}
        if unknown_keys:
            raise ValueError(
                f"{path}: profile {name!r} has unknown keys: {sorted(unknown_keys)}"
            )
        parents = definition.get("extends", [])
        packages = definition.get("packages", [])
        if not isinstance(parents, list) or not all(
            isinstance(parent, str) and parent for parent in parents
        ):
            raise ValueError(f"{path}: profile {name!r} extends must be a string list")
        if parents != sorted(set(parents)):
            raise ValueError(
                f"{path}: profile {name!r} parents must be sorted and unique"
            )
        if not isinstance(packages, list) or not packages:
            raise ValueError(f"{path}: profile {name!r} must contain packages")
        if packages != sorted(set(packages)):
            raise ValueError(
                f"{path}: profile {name!r} packages must be sorted and unique"
            )
        for package in packages:
            validate_package(package)
        definitions[name] = (parents, packages)

    resolved: dict[str, list[str]] = {}

    def resolve(name: str, stack: tuple[str, ...] = ()) -> list[str]:
        if name in resolved:
            return resolved[name]
        if name not in definitions:
            raise ValueError(f"{path}: unknown parent profile {name!r}")
        if name in stack:
            cycle = " -> ".join((*stack, name))
            raise ValueError(f"{path}: profile inheritance cycle: {cycle}")
        parents, packages = definitions[name]
        combined = set(packages)
        for parent in parents:
            combined.update(resolve(parent, (*stack, name)))
        resolved[name] = sorted(combined)
        return resolved[name]

    for name in definitions:
        resolve(name)
    return resolved


def validate_package(package: object) -> str:
    if not isinstance(package, str) or not PACKAGE_RE.fullmatch(package):
        raise ValueError(f"invalid apt package name: {package!r}")
    return package


def resolve_packages(
    profile_names: Sequence[str],
    extras: Sequence[str],
    *,
    profiles: dict[str, list[str]] | None = None,
) -> list[str]:
    available = profiles if profiles is not None else load_profiles()
    if not profile_names:
        raise ValueError("at least one --profile is required")

    packages: set[str] = set()
    for name in profile_names:
        if name not in available:
            choices = ", ".join(sorted(available))
            raise ValueError(f"unknown profile {name!r}; choose from: {choices}")
        packages.update(available[name])
    packages.update(validate_package(package) for package in extras)
    return sorted(packages)


def run_with_retry(
    command: Sequence[str],
    *,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    sleeper: Callable[[float], None] = time.sleep,
    attempts: int = MAX_ATTEMPTS,
    capture_output: bool = False,
    retry_if: Callable[[subprocess.CompletedProcess[str]], bool] | None = None,
) -> subprocess.CompletedProcess[str]:
    for attempt in range(1, attempts + 1):
        kwargs: dict[str, object] = {"check": False, "text": True}
        if capture_output:
            kwargs.update(stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        result = runner(command, **kwargs)
        if result.returncode == 0:
            return result
        output = result.stdout or ""
        if output:
            print(output.rstrip(), file=sys.stderr)
        if attempt == attempts or (retry_if is not None and not retry_if(result)):
            raise subprocess.CalledProcessError(
                result.returncode,
                command,
                output=output,
            )
        delay = 5 * attempt
        print(
            f"apt command failed (attempt {attempt}/{attempts}); "
            f"retrying in {delay}s",
            file=sys.stderr,
        )
        sleeper(delay)
    raise AssertionError("unreachable")


def failed_apt_403_hosts(output: str) -> set[str]:
    """Return repository hosts named by apt as HTTP 403 failures."""

    hosts: set[str] = set()
    pending_error_hosts: set[str] = set()
    for line in output.splitlines():
        stripped = line.strip()
        line_hosts = {
            urllib.parse.urlparse(url.rstrip(".,;:)")).hostname
            for url in URL_RE.findall(stripped)
        }
        line_hosts.discard(None)
        if stripped.startswith("Err:"):
            pending_error_hosts = {str(host) for host in line_hosts}
        if "403" not in stripped:
            continue
        if "Failed to fetch" in stripped or stripped.startswith("Err:"):
            hosts.update(str(host) for host in line_hosts)
        elif pending_error_hosts:
            hosts.update(pending_error_hosts)
        pending_error_hosts.clear()
    return hosts


def is_quarantinable_apt_failure(result: subprocess.CompletedProcess[str]) -> bool:
    hosts = failed_apt_403_hosts(result.stdout or "")
    return bool(hosts) and hosts <= UNRELATED_THIRD_PARTY_APT_HOSTS


def _active_source_uris(path: pathlib.Path) -> set[str]:
    text = path.read_text(encoding="utf-8")
    if path.suffix == ".list":
        return {
            match.group("uri")
            for line in text.splitlines()
            if (match := ONE_LINE_SOURCE_RE.match(line))
        }

    uris: set[str] = set()
    for paragraph in re.split(r"\n\s*\n", text):
        if re.search(r"(?im)^Enabled:\s*no\s*$", paragraph):
            continue
        lines = paragraph.splitlines()
        for index, line in enumerate(lines):
            match = re.match(r"(?i)^URIs:\s*(.*)$", line)
            if not match:
                continue
            values = [match.group(1)]
            for continuation in lines[index + 1 :]:
                if not continuation.startswith((" ", "\t")):
                    break
                values.append(continuation.strip())
            uris.update(" ".join(values).split())
    return uris


def _source_uri_host(uri: str) -> str | None:
    if urllib.parse.urlparse(uri).scheme not in {"http", "https"}:
        return None
    return urllib.parse.urlparse(uri).hostname


def _apt_source_files(source_root: pathlib.Path) -> list[pathlib.Path]:
    files: list[pathlib.Path] = []
    primary = source_root / "sources.list"
    if primary.is_file():
        files.append(primary)
    parts = source_root / "sources.list.d"
    if parts.is_dir():
        files.extend(
            path
            for path in sorted(parts.iterdir())
            if path.is_file() and path.suffix in APT_SOURCE_SUFFIXES
        )
    return files


@contextlib.contextmanager
def quarantined_apt_sources(
    source_root: pathlib.Path,
    failing_hosts: set[str],
) -> Iterator[tuple[list[str], list[pathlib.Path]]]:
    """Yield apt options excluding dedicated files for exact failing hosts.

    A file that mixes a failing third-party host with any other repository is
    rejected rather than partially rewritten. This keeps distribution and
    profile-required repositories byte-for-byte intact.
    """

    source_files = _apt_source_files(source_root)
    quarantined: list[pathlib.Path] = []
    mixed: list[pathlib.Path] = []
    for path in source_files:
        uris = _active_source_uris(path)
        hosts = {_source_uri_host(uri) for uri in uris}
        if not hosts.intersection(failing_hosts):
            continue
        if uris and all(_source_uri_host(uri) in failing_hosts for uri in uris):
            quarantined.append(path)
        else:
            mixed.append(path)
    if mixed:
        joined = ", ".join(str(path) for path in mixed)
        raise RuntimeError(
            "refusing to quarantine apt source file(s) that also contain "
            f"preserved repositories: {joined}"
        )
    if not quarantined:
        hosts = ", ".join(sorted(failing_hosts))
        raise RuntimeError(
            "apt reported failing third-party host(s) but no dedicated source "
            f"file was found: {hosts}"
        )

    with tempfile.TemporaryDirectory(prefix="pulp-apt-sources-") as directory:
        temporary_root = pathlib.Path(directory)
        temporary_parts = temporary_root / "sources.list.d"
        temporary_parts.mkdir()
        temporary_primary = temporary_root / "sources.list"
        primary = source_root / "sources.list"
        if primary.is_file() and primary not in quarantined:
            shutil.copyfile(primary, temporary_primary)
        else:
            temporary_primary.touch()
        for path in source_files:
            if path == primary or path in quarantined:
                continue
            shutil.copyfile(path, temporary_parts / path.name)
        options = [
            "-o",
            f"Dir::Etc::sourcelist={temporary_primary}",
            "-o",
            f"Dir::Etc::sourceparts={temporary_parts}",
        ]
        yield options, quarantined


def install(
    packages: Sequence[str],
    *,
    source_root: pathlib.Path = pathlib.Path("/etc/apt"),
    platform: str = sys.platform,
    euid: int | None = None,
    subprocess_runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    sleeper: Callable[[float], None] = time.sleep,
) -> None:
    if platform != "linux":
        raise RuntimeError("Linux dependency installation is supported only on Linux")
    effective_euid = os.geteuid() if euid is None else euid
    prefix = [] if effective_euid == 0 else ["sudo"]
    env = os.environ.copy()
    env["DEBIAN_FRONTEND"] = "noninteractive"

    def runner(command: Sequence[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
        return subprocess_runner(command, env=env, **kwargs)

    try:
        run_with_retry(
            [*prefix, "apt-get", "update"],
            runner=runner,
            sleeper=sleeper,
            capture_output=True,
            retry_if=lambda result: not is_quarantinable_apt_failure(result),
        )
    except subprocess.CalledProcessError as error:
        failing_hosts = failed_apt_403_hosts(str(error.output or ""))
        if not failing_hosts or not failing_hosts <= UNRELATED_THIRD_PARTY_APT_HOSTS:
            raise
        with quarantined_apt_sources(source_root, failing_hosts) as (
            apt_options,
            quarantined,
        ):
            print(
                "apt source quarantine: HTTP 403 from unrelated host(s) "
                f"{', '.join(sorted(failing_hosts))}; temporarily excluding "
                + ", ".join(str(path) for path in quarantined),
                file=sys.stderr,
            )
            run_with_retry(
                [*prefix, "apt-get", *apt_options, "update"],
                runner=runner,
                sleeper=sleeper,
                capture_output=True,
            )
            run_with_retry(
                [*prefix, "apt-get", *apt_options, "install", "-y", *packages],
                runner=runner,
                sleeper=sleeper,
            )
            return
    run_with_retry(
        [*prefix, "apt-get", "install", "-y", *packages],
        runner=runner,
        sleeper=sleeper,
    )


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", action="append", default=[])
    parser.add_argument(
        "--extra-packages",
        action="append",
        default=[],
        metavar="PACKAGES",
        help="Whitespace-separated lane-specific packages; may be repeated.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the resolved package set as JSON without running apt.",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    extras = [
        package
        for value in args.extra_packages
        for package in shlex.split(value)
    ]
    try:
        packages = resolve_packages(args.profile, extras)
        if args.dry_run:
            print(json.dumps(packages, indent=2))
        else:
            install(packages)
    except (TypeError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"install-linux-build-deps: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
