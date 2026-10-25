#!/usr/bin/env python3
"""Keep the release-please configuration a projection of versioning.json.

release-please assigns Pulp's versions (see docs/guides/versioning.md § Release
PR). It decides which commits belong to a package by directory: a package at
the repository root "." receives every commit, minus commits whose files ALL
sit under one of its `exclude-paths` directories. There is no include-glob, so
it cannot read `versioning.json`'s `trigger_paths` directly.

Each surface is therefore its own release-please configuration with a single
root package, and its `exclude-paths` is DERIVED here: every directory holding
no tracked file that matches the surface's `trigger_paths`, taken at the
shallowest level where that is true. That reproduces the trigger paths at
directory granularity. The remainder errs toward releasing: a commit touching
only top-level files, or a non-trigger file in a directory that also holds
trigger files (most of `tools/scripts/`), counts for the surface where the
path heuristic would not have.

Modes:
    release_please_config.py --check   # exit 1 when a config or manifest is unsafe
    release_please_config.py --write   # rewrite each config's exclude-paths

`--check` fails on three things:

* drift that would lose a release: an excluded directory that now holds a
  trigger file. A directory the derivation would add (a new top-level or
  `tools/` directory with no trigger files) only over-releases until someone
  runs `--write`, so it is reported and passes; a PR adding one is not made to
  regenerate release config;
* a manifest that disagrees with its surface's version files. A manifest
  behind the files would make release-please walk back to an older release
  and propose a version below main's, so the release workflow and the PR gate
  both refuse to proceed;
* a bootstrap `last-release-sha` that is neither the commit of the tag the
  manifest names nor an ancestor of it.

Exit codes: 0 = consistent, 1 = drift, 2 = could not run.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from gate_common import matches_any  # noqa: E402
from version_bump_surfaces import load_config, read_version  # noqa: E402

ROOT_PACKAGE = "."


def tracked_files(repo: Path) -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "-z"], cwd=repo, capture_output=True, check=True
    ).stdout.decode("utf-8")
    return [path for path in out.split("\0") if path]


def derive_exclude_paths(trigger_paths: list[str], files: list[str]) -> list[str]:
    """The shallowest directories that hold no file matching `trigger_paths`."""
    holds_trigger: set[str] = set()
    directories: set[str] = set()
    for path in files:
        parts = path.split("/")
        parents = ["/".join(parts[:i]) for i in range(1, len(parts))]
        directories.update(parents)
        if matches_any(path, trigger_paths):
            holds_trigger.update(parents)
    return sorted(
        directory
        for directory in directories
        if directory not in holds_trigger
        and ("/" not in directory or directory.rsplit("/", 1)[0] in holds_trigger)
    )


def _load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"release-please-config: cannot read {path}: {error}") from error


def _dump_json(path: Path, data: dict) -> None:
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def _tag_name(rp_config: dict, version: str | None) -> str:
    package = rp_config.get("packages", {}).get(ROOT_PACKAGE, {})

    def option(key: str, default):
        return package.get(key, rp_config.get(key, default))

    prefix = ""
    if option("include-component-in-tag", True) and option("component", ""):
        prefix = option("component", "") + option("tag-separator", "-")
    return prefix + ("v" if option("include-v-in-tag", True) else "") + str(version)


def _check_pin(repo: Path, surface: str, config: str, pinned: str, tag: str) -> list[str]:
    """`last-release-sha` bootstraps the first run: it must not lie past the release."""
    found = subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{tag}^{{commit}}"],
                           cwd=repo, capture_output=True, text=True, encoding="utf-8")
    if found.returncode != 0:
        return []  # tag not fetched here, or not cut yet; nothing to compare
    ancestor = subprocess.run(["git", "merge-base", "--is-ancestor", pinned, found.stdout.strip()],
                              cwd=repo, capture_output=True)
    if ancestor.returncode != 0:
        return [f"[{surface}] {config} last-release-sha {pinned[:12]} is not {tag} or an "
                "ancestor of it, so release-please would skip released history or recount it"]
    return []


def check(repo: Path, config_path: Path, write: bool = False) -> list[str]:
    """Drift messages for every surface (empty when consistent)."""
    config = load_config(config_path)
    raw = _load_json(config_path)
    files = tracked_files(repo)
    problems: list[str] = []
    for surface in config.surfaces:
        mapping = raw["surfaces"][surface.name].get("release_please")
        if not mapping:
            problems.append(f"[{surface.name}] versioning.json names no release_please config")
            continue
        rp_config_path = repo / mapping["config"]
        rp_manifest_path = repo / mapping["manifest"]
        rp_config = _load_json(rp_config_path)
        package = rp_config.get("packages", {}).get(ROOT_PACKAGE)
        if package is None or len(rp_config["packages"]) != 1:
            problems.append(
                f"[{surface.name}] {mapping['config']} must hold exactly one "
                f'package, at "{ROOT_PACKAGE}"'
            )
            continue

        expected = derive_exclude_paths(surface.trigger_paths, files)
        if write and package.get("exclude-paths") != expected:
            package["exclude-paths"] = expected
            _dump_json(rp_config_path, rp_config)
        have = package.get("exclude-paths") or []
        triggers = [path for path in files if matches_any(path, surface.trigger_paths)]
        unsafe = sorted(
            directory for directory in have
            if any(path.startswith(directory + "/") for path in triggers)
        )
        if unsafe:
            problems.append(
                f"[{surface.name}] {mapping['config']} excludes {unsafe}, which now "
                "hold trigger files, so release-please would not release changes to "
                "them; run tools/scripts/release_please_config.py --write"
            )
        missing = sorted(set(expected) - set(have))
        if missing:
            print(
                f"release-please-config: [{surface.name}] {mapping['config']} could "
                f"also exclude {missing} (until then a typed commit touching only "
                "them releases this surface); run release_please_config.py --write",
                file=sys.stderr,
            )

        recorded = _load_json(rp_manifest_path).get(ROOT_PACKAGE)
        pinned = rp_config.get("last-release-sha")
        if pinned:
            problems.extend(_check_pin(repo, surface.name, mapping["config"], pinned,
                                       _tag_name(rp_config, recorded)))
        for version_file in surface.version_files:
            current = read_version(repo, version_file)
            if current != recorded:
                problems.append(
                    f"[{surface.name}] {mapping['manifest']} records {recorded} but "
                    f"{version_file.path} carries {current}; the manifest must name "
                    "the version on main or release-please proposes the wrong next one"
                )
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--write", action="store_true")
    parser.add_argument("--repo", type=Path, default=Path("."))
    parser.add_argument("--config", type=Path, default=None)
    args = parser.parse_args(argv)
    repo = args.repo.resolve()
    config_path = args.config or repo / "tools" / "scripts" / "versioning.json"
    try:
        problems = check(repo, config_path, write=args.write)
    except (OSError, KeyError, subprocess.CalledProcessError) as error:
        print(f"release-please-config: could not run: {error}", file=sys.stderr)
        return 2
    for problem in problems:
        print(f"release-please-config: {problem}", file=sys.stderr)
    if problems:
        return 1
    print("release-please-config: configs and manifests match versioning.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
