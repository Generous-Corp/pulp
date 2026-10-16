#!/usr/bin/env python3
"""Generate the toolchain-owned TOML fragments from one authority."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import tomllib
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools/toolchain/manifest.toml"
BEGIN = "# BEGIN GENERATED PULP TOOLCHAIN (tools/toolchain/manifest.toml)"
END = "# END GENERATED PULP TOOLCHAIN"

# Exact versions only. A selector such as "homebrew", "stable", "latest" or a
# bare "3.12" names a moving target, so two hosts reading the same manifest can
# install different tools. Xcode versions are major.minor by Apple's scheme.
EXACT = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
EXACT_XCODE = re.compile(r"^[0-9]+\.[0-9]+(\.[0-9]+)?$")

# Which pins mise installs, from what, and with which lock exceptions lives in
# the manifest's [mise.tools.*] tables, where a reviewer sees it. Rust is not
# one of them: rustup installs it and cargo reads rust-toolchain.toml.
LOCK_DIGEST_RE = re.compile(r'(?m)^lock_sha256 = "[^"]*"$')
# The pinned tools live in mise's environment-scoped config, loaded only when
# MISE_ENV=toolchain. A shell that activates mise in a Pulp checkout without
# opting in is untouched: no PATH change and no warnings from an older mise
# that cannot read this lock. pulp build, the generators and fleet hosts opt
# in explicitly. The base .mise.toml keeps the settings and the tasks.
MISE_ENV = "toolchain"
MISE_CONFIG = ".mise.toml"
MISE_TOOLS_CONFIG = f".mise.{MISE_ENV}.toml"
MISE_LOCK = f"mise.{MISE_ENV}.lock"
# What tools/toolchain/check_env.py compares a host against. check_env runs
# before the pinned Python exists, on whatever python3 a host has (3.9 on a
# stock Mac), which has no tomllib; it reads this JSON instead of the TOML.
EXPECTED = "tools/toolchain/expected.json"
# The tools check_env reports, by [toolchain] key. Xcode and tart are absent:
# Xcode is recorded by the golden receipt, and tart is a host tool tartci owns.
CHECKED = ("python", "cmake", "ninja", "node", "rust", "clang_format")
# Shipyard's pin. tools/shipyard.toml keeps its prose and the two keys Shipyard
# reads (version, repo), which generate.py owns; the asset digests go into a
# checksum file tools/install-shipyard.sh reads without a TOML parser.
SHIPYARD_PIN = "tools/shipyard.toml"
SHIPYARD_SUMS = "tools/toolchain/shipyard.sha256"
# The upstream installer script install-shipyard.sh runs; its digest is the
# last line of the checksum file under this name.
SHIPYARD_INSTALLER = "install.sh"
SHIPYARD_TAG = re.compile(r"^v[0-9]+\.[0-9]+\.[0-9]+$")

TARGETS = {
    "macos": {"keys": ("xcode", "xcode_build", "cmake", "ninja", "python", "node", "rust", "rust_targets", "tart", "clang_format"), "manager": "brew"},
    "intel": {"keys": ("rust", "rust_targets"), "manager": None},
    "linux": {"keys": ("cmake", "python", "node"), "manager": "apt"},
    "windows": {"keys": ("cmake", "python", "node"), "manager": "choco"},
}


def source_bytes(path: Path = SOURCE) -> bytes:
    return path.read_bytes()


def load(path: Path = SOURCE) -> tuple[dict, str]:
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise ValueError(f"cannot read toolchain authority: {exc}") from exc
    if data.get("schema") != 1 or not isinstance(data.get("toolchain"), dict):
        raise ValueError("toolchain authority must have schema=1 and [toolchain]")
    required = ("xcode", "cmake", "ninja", "python", "node", "rust", "tart", "clang_format")
    missing = [key for key in required if key not in data["toolchain"]]
    if missing:
        raise ValueError("toolchain authority missing: " + ", ".join(missing))
    _require_exact(data)
    return data, hashlib.sha256(source_bytes(path)).hexdigest()


def mise_tools(data: dict) -> dict[str, dict]:
    return data["mise"]["tools"]


def mise_name(key: str, spec: dict) -> str:
    """The [tools] key mise uses: the registry short name, or the full backend
    for a backend (conda:, pipx:) that has no registry entry for the tool."""
    backend = spec["backend"]
    return key.replace("_", "-") if backend.startswith(("core:", "aqua:")) else backend


def locked_platforms(data: dict, spec: dict) -> list[str]:
    unlocked = spec.get("unlocked", {})
    return [p for p in data["mise"]["lock_platforms"] if p not in unlocked]


def os_selector(data: dict, spec: dict) -> list[str] | None:
    """mise `os` values covering exactly the tool's locked platforms."""
    every = data["mise"]["lock_platforms"]
    wanted = set(locked_platforms(data, spec))
    if wanted == set(every):
        return None
    out: list[str] = []
    for os_name in sorted({p.split("-", 1)[0] for p in every}):
        mine = [p for p in every if p.split("-", 1)[0] == os_name]
        if all(p in wanted for p in mine):
            out.append(os_name)
        else:
            out += [p.replace("-", "/", 1) for p in mine if p in wanted]
    return out


def _require_exact(data: dict) -> None:
    tc = data["toolchain"]
    mise = data.get("mise")
    if not isinstance(mise, dict) or not isinstance(mise.get("tools"), dict) or not mise["tools"]:
        raise ValueError("toolchain authority must have [mise] with [mise.tools.*] tables")
    pinned = [key for key in ("cmake", "ninja", "python", "node", "rust", "tart", "clang_format")]
    floating = [
        f"{key} = {tc.get(key)!r}"
        for key in pinned
        if not isinstance(tc.get(key), str) or not EXACT.match(tc[key])
    ]
    if not isinstance(tc.get("xcode"), str) or not EXACT_XCODE.match(tc["xcode"]):
        floating.append(f"xcode = {tc.get('xcode')!r}")
    if not isinstance(tc.get("xcode_build"), str) or not re.match(r"^[0-9]{2}[A-Z][0-9]+[a-z]?$", tc["xcode_build"]):
        floating.append(f"xcode_build = {tc.get('xcode_build')!r}")
    if not isinstance(mise.get("version"), str) or not re.match(r"^[0-9]{4}\.[0-9]+\.[0-9]+$", mise["version"]):
        floating.append(f"mise.version = {mise.get('version')!r}")
    if not isinstance(mise.get("source_commit"), str) or not re.match(r"^[0-9a-f]{40}$", mise["source_commit"]):
        floating.append(f"mise.source_commit = {mise.get('source_commit')!r}")
    if floating:
        raise ValueError("toolchain authority has non-exact pins: " + "; ".join(floating))
    evidence = data.get("evidence", {})
    for key in ("xcode",) + tuple(pinned):
        row = evidence.get(key)
        if not isinstance(row, dict) or row.get("status") not in ("observed", "target") or not row.get("source"):
            raise ValueError(f"evidence.{key} must say status = 'observed' or 'target' and name its source")
    platforms = mise.get("lock_platforms")
    if not isinstance(platforms, list) or not platforms or platforms != sorted(set(platforms)):
        raise ValueError("mise.lock_platforms must be a non-empty, sorted, unique list")
    if mise.get("enforcement") not in ("warn", "enforce"):
        raise ValueError("mise.enforcement must be 'warn' or 'enforce'")
    if not isinstance(mise.get("lock_sha256"), str):
        raise ValueError("mise.lock_sha256 must be a string")
    binary = mise.get("binary")
    if not isinstance(binary, dict) or not isinstance(binary.get("path"), str) or mise["version"] not in binary["path"]:
        raise ValueError("mise.binary.path must name the installed pinned binary, with its version in the path")
    digests = binary.get("sha256")
    if not isinstance(digests, dict) or not digests or any(
        p not in platforms or not isinstance(d, str) or not re.match(r"^[0-9a-f]{64}$", d)
        for p, d in digests.items()
    ):
        raise ValueError("mise.binary.sha256 must map lock platforms to 64-hex sha256 digests")
    if "rust" in mise["tools"]:
        raise ValueError("rust must not be a [mise.tools] entry: rustup installs it, rust-toolchain.toml pins it")
    for key, spec in mise["tools"].items():
        if key not in pinned:
            raise ValueError(f"mise.tools.{key} has no [toolchain] pin")
        if not isinstance(spec.get("backend"), str) or ":" not in spec["backend"]:
            raise ValueError(f"mise.tools.{key}.backend must be a mise backend such as aqua:owner/repo")
        prefixes = spec.get("url_prefixes")
        if not isinstance(prefixes, list) or not prefixes or not all(
            isinstance(x, str) and x.startswith("https://") and x.endswith("/") for x in prefixes
        ):
            raise ValueError(f"mise.tools.{key}.url_prefixes must list https:// prefixes ending in /")
        backend = spec["backend"]
        if backend.startswith("aqua:"):
            repo = backend.split(":", 1)[1]
            for prefix in prefixes:
                if f"/{repo}/" not in prefix:
                    raise ValueError(f"mise.tools.{key}: {prefix} is not the {backend} repository")
        unlocked = spec.get("unlocked", {})
        if not isinstance(unlocked, dict) or any(
            p not in platforms or not isinstance(r, str) or not r for p, r in unlocked.items()
        ):
            raise ValueError(f"mise.tools.{key}.unlocked must map lock platforms to a reason")
    _require_shipyard(data.get("shipyard"))
    prefixes = data.get("npm", {}).get("prefixes")
    if not isinstance(prefixes, list) or not prefixes:
        raise ValueError("npm.prefixes must be a non-empty list of lockfile directories")


def _require_shipyard(shipyard: object) -> None:
    if not isinstance(shipyard, dict):
        raise ValueError("toolchain authority must have [shipyard] (version, repo, assets)")
    if not isinstance(shipyard.get("version"), str) or not SHIPYARD_TAG.match(shipyard["version"]):
        raise ValueError(f"shipyard.version must be an exact release tag such as v0.269.1, not {shipyard.get('version')!r}")
    if not isinstance(shipyard.get("repo"), str) or not re.match(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$", shipyard["repo"]):
        raise ValueError("shipyard.repo must be owner/name")
    if not isinstance(shipyard.get("installer_sha256"), str) or not re.match(r"^[0-9a-f]{64}$", shipyard["installer_sha256"]):
        raise ValueError("shipyard.installer_sha256 must be the 64-hex sha256 of install.sh at the pinned tag")
    assets = shipyard.get("assets")
    if not isinstance(assets, dict) or not assets or any(
        not re.match(r"^shipyard[A-Za-z0-9_.-]*$", name) or not isinstance(d, str) or not re.match(r"^[0-9a-f]{64}$", d)
        for name, d in assets.items()
    ):
        raise ValueError("shipyard.assets must map release asset names to 64-hex sha256 digests")


def toml_value(value: object) -> str:
    if isinstance(value, list):
        return "[" + ", ".join('"' + str(item) + '"' for item in value) + "]"
    if isinstance(value, bool):
        return "true" if value else "false"
    return '"' + str(value) + '"'


def _section_bounds(lines: list[str], section: str) -> tuple[int, int]:
    header = f"[{section}]"
    begin = next((i for i, line in enumerate(lines) if line.strip() == header), None)
    if begin is None:
        raise ValueError(f"missing [{section}] section")
    end = len(lines)
    for i in range(begin + 1, len(lines)):
        if lines[i].startswith("[") and lines[i].rstrip().endswith("]"):
            end = i
            break
    return begin, end


def _set_owned_keys(lines: list[str], section: str, values: dict[str, object], keys: tuple[str, ...]) -> None:
    begin, end = _section_bounds(lines, section)
    for key in keys:
        value = values.get(key)
        if value is None or value == "absent":
            continue
        replacement = f"{key} = {toml_value(value)}"
        found = False
        for i in range(begin + 1, end):
            if re.match(rf"^{re.escape(key)}\s*=", lines[i]):
                lines[i] = replacement + "\n"
                found = True
                break
        if not found:
            lines.insert(begin + 1, replacement + "\n")
            end += 1


def _set_packages(lines: list[str], manager: str, packages: list[str]) -> None:
    begin, end = _section_bounds(lines, manager)
    start = next((i for i in range(begin + 1, end) if re.match(r"^packages\s*=\s*\[", lines[i])), None)
    if start is None:
        lines.insert(begin + 1, "packages = [\n" + "".join(f'  "{item}",\n' for item in packages) + "]\n")
        return
    finish = next((i for i in range(start + 1, end) if lines[i].strip() == "]"), None)
    if finish is None:
        raise ValueError(f"{manager}.packages is unterminated")
    comments = [line for line in lines[start + 1 : finish] if line.lstrip().startswith("#")]
    generated = ["packages = [\n"] + [f'  "{item}",\n' for item in packages] + comments + ["]\n"]
    lines[start : finish + 1] = generated


def update_file(path: Path, data: dict, digest: str, platform: str, check: bool) -> bool:
    text = path.read_text(encoding="utf-8")
    marker = re.compile(re.escape(BEGIN) + r"\n.*?" + re.escape(END) + r"\n", re.S)
    if not marker.search(text):
        raise ValueError(f"{path}: generated toolchain block is missing")
    marker_text = f"{BEGIN}\n# source_sha256 = {digest}\n# generated_platform = {platform}\n{END}\n"
    updated = marker.sub(marker_text, text, count=1)
    lines = updated.splitlines(keepends=True)
    target = data.get("platform", {}).get(platform, {})
    common = data["toolchain"]
    values = {key: target.get(key, common.get(key)) for key in TARGETS[platform]["keys"]}
    _set_owned_keys(lines, "toolchain", values, TARGETS[platform]["keys"])
    manager = TARGETS[platform]["manager"]
    if manager:
        packages = data.get("packages", {}).get(manager)
        if not isinstance(packages, list) or not packages:
            raise ValueError(f"packages.{manager} must be a non-empty list")
        _set_packages(lines, manager, packages)
    result = "".join(lines)
    if check and result != text:
        raise ValueError(f"{path}: generated toolchain block or owned fields are stale")
    if not check:
        path.write_text(result, encoding="utf-8")
    return result != text

MISE_IMAGE = ".shipyard/vm-image.mise.toml"


def render_mise_image_block(data: dict, digest: str) -> str:
    """The generated [mise] table of the pinned-toolchain golden layer
    (tools/ci/mise-golden-layer.sh reads it)."""
    mise, tc = data["mise"], data["toolchain"]
    lines = [
        BEGIN,
        f"# source_sha256 = {digest}",
        "[mise]",
        f'version = "{mise["version"]}"',
        f'path = "{mise["binary"]["path"]}"',
        f'sha256 = "{mise["binary"]["sha256"]["macos-arm64"]}"',
        f'rust = "{tc["rust"]}"',
        f"rust_components = {toml_value(tc.get('rust_components', []))}",
        f"rust_targets = {toml_value(tc.get('rust_targets', []))}",
        END,
        "",
    ]
    return "\n".join(lines)


def update_mise_image(path: Path, data: dict, digest: str, check: bool) -> bool:
    text = path.read_text(encoding="utf-8")
    marker = re.compile(re.escape(BEGIN) + r"\n.*?" + re.escape(END) + r"\n", re.S)
    if not marker.search(text):
        raise ValueError(f"{path}: generated toolchain block is missing")
    result = marker.sub(lambda _m: render_mise_image_block(data, digest), text, count=1)
    if check and result != text:
        raise ValueError(
            f"{path}: the generated [mise] block differs from tools/toolchain/manifest.toml "
            "(stale or hand-edited); fix: python3 tools/toolchain/generate.py"
        )
    if not check:
        path.write_text(result, encoding="utf-8")
    return result != text


def _mise_tool_line(data: dict, key: str, spec: dict) -> str:
    version = data["toolchain"][key]
    name = mise_name(key, spec)
    lhs = name if re.match(r"^[A-Za-z0-9_-]+$", name) else f'"{name}"'
    oses = os_selector(data, spec)
    if oses is None:
        return f'{lhs} = "{version}"'
    return f'{lhs} = {{ version = "{version}", os = {toml_value(oses)} }}'


def render_tools_config(data: dict, digest: str) -> str:
    """The whole of .mise.toolchain.toml: [tools], and min_version once enforced.

    In warn mode there is no min_version, so an older mise prints no upgrade
    warning; version reports belong to pulp build and the generators.
    """
    mise = data["mise"]
    lines = [
        "# GENERATED by tools/toolchain/generate.py from tools/toolchain/manifest.toml.",
        "# Do not edit. Loaded only with MISE_ENV=toolchain; install with",
        "#   MISE_ENV=toolchain mise install --locked",
        f"# source_sha256 = {digest}",
        f"# lock_sha256 = {mise['lock_sha256']}",
    ]
    if mise["enforcement"] == "enforce":
        lines.append(f'min_version = "{mise["version"]}"')
    lines += ["", "[tools]"]
    lines += [_mise_tool_line(data, key, spec) for key, spec in mise_tools(data).items()]
    return "\n".join(lines) + "\n"


def render_mise_block(data: dict, digest: str) -> str:
    """The generated block of the base .mise.toml: the npm task."""
    lines = [
        BEGIN,
        f"# source_sha256 = {digest}",
        "[tasks.npm-deps]",
        'description = "Install project npm dependencies from the committed lockfiles (npm ci)"',
        "run = [",
    ]
    lines += [
        f'  "npm ci --prefix {prefix} --no-audit --no-fund",'
        for prefix in data["npm"]["prefixes"]
    ]
    lines += ["]", END, ""]
    return "\n".join(lines)


def update_tools_config(path: Path, data: dict, digest: str, check: bool) -> bool:
    text = path.read_text(encoding="utf-8") if path.exists() else ""
    result = render_tools_config(data, digest)
    if check and result != text:
        raise ValueError(
            f"{path}: differs from tools/toolchain/manifest.toml (stale or hand-edited); "
            "fix: python3 tools/toolchain/generate.py"
        )
    if not check:
        path.write_text(result, encoding="utf-8")
    return result != text


def render_expected(data: dict, digest: str) -> str:
    """tools/toolchain/expected.json: the subset check_env needs, as JSON."""
    mise = data["mise"]
    overrides: dict[str, dict[str, str]] = {}
    for name, table in sorted(data.get("platform", {}).items()):
        picked = {k: v for k, v in table.items() if k in CHECKED and v in ("system", "absent")}
        if picked:
            overrides[name] = picked
    doc = {
        "_generated": "by tools/toolchain/generate.py from tools/toolchain/manifest.toml; do not edit",
        "schema": 1,
        "source_sha256": digest,
        "enforcement": mise["enforcement"],
        "tools": {key: data["toolchain"][key] for key in CHECKED},
        "platform_overrides": overrides,
        "mise": {
            "version": mise["version"],
            "path": mise["binary"]["path"],
            "sha256": dict(sorted(mise["binary"]["sha256"].items())),
        },
        "npm_prefixes": list(data["npm"]["prefixes"]),
    }
    return json.dumps(doc, indent=2) + "\n"


def update_expected(path: Path, data: dict, digest: str, check: bool) -> bool:
    text = path.read_text(encoding="utf-8") if path.exists() else ""
    result = render_expected(data, digest)
    if check and result != text:
        raise ValueError(
            f"{path}: differs from tools/toolchain/manifest.toml (stale or hand-edited); "
            "fix: python3 tools/toolchain/generate.py"
        )
    if not check:
        path.write_text(result, encoding="utf-8")
    return result != text


def render_shipyard_sums(data: dict, digest: str) -> str:
    shipyard = data["shipyard"]
    lines = [
        "# GENERATED by tools/toolchain/generate.py from tools/toolchain/manifest.toml; do not edit.",
        f"# source_sha256 = {digest}",
        f"# Shipyard {shipyard['version']} release assets ({shipyard['repo']}), sha256sum format.",
    ]
    lines += [f"{sha}  {name}" for name, sha in sorted(shipyard["assets"].items())]
    lines.append(f"{shipyard['installer_sha256']}  {SHIPYARD_INSTALLER}")
    return "\n".join(lines) + "\n"


def update_shipyard_sums(path: Path, data: dict, digest: str, check: bool) -> bool:
    text = path.read_text(encoding="utf-8") if path.exists() else ""
    result = render_shipyard_sums(data, digest)
    if check and result != text:
        raise ValueError(
            f"{path}: differs from tools/toolchain/manifest.toml (stale or hand-edited); "
            "fix: python3 tools/toolchain/generate.py"
        )
    if not check:
        path.write_text(result, encoding="utf-8")
    return result != text


def update_shipyard_pin(path: Path, data: dict, check: bool) -> bool:
    """Own the `version` and `repo` keys of tools/shipyard.toml's [shipyard] table.

    The rest of the file (its rationale prose) stays hand-written. A pin edited
    only here, for example by `shipyard pin bump`, is refused in check mode with
    the command that moves the manifest instead.
    """
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    begin, end = _section_bounds(lines, "shipyard")
    want = {"version": data["shipyard"]["version"], "repo": data["shipyard"]["repo"]}
    for key, value in want.items():
        index = next((i for i in range(begin + 1, end) if re.match(rf"^{key}\s*=", lines[i])), None)
        if index is None:
            raise ValueError(f"{path}: [shipyard] has no {key} line")
        have = re.match(rf'^{key}\s*=\s*"([^"]*)"', lines[index])
        if have and have.group(1) == value:
            continue
        if check:
            found = have.group(1) if have else lines[index].strip()
            raise ValueError(
                f"{path}: [shipyard] {key} is {found!r} but tools/toolchain/manifest.toml pins {value!r}; "
                "the manifest is the authority. fix: python3 tools/toolchain/generate.py --shipyard <tag> "
                "(it takes the version and the asset digests from the release), or revert this edit"
            )
        lines[index] = f'{key} = "{value}"\n'
    result = "".join(lines)
    if not check:
        path.write_text(result, encoding="utf-8")
    return result != text


def _tag_key(tag: str) -> tuple[int, ...]:
    return tuple(int(part) for part in tag.lstrip("v").split("."))


def _fetch(url: str) -> bytes:
    try:
        with urllib.request.urlopen(url, timeout=60) as response:  # noqa: S310 - fixed https URL
            return response.read()
    except OSError as exc:
        raise ValueError(f"cannot read {url}: {exc}") from exc


def fetch_shipyard_installer_sha256(repo: str, tag: str) -> str:
    """sha256 of install.sh at the release tag. Network read."""
    return hashlib.sha256(_fetch(f"https://raw.githubusercontent.com/{repo}/refs/tags/{tag}/install.sh")).hexdigest()


def fetch_shipyard_assets(repo: str, tag: str) -> dict[str, str]:
    """Asset name -> sha256 from the release's own checksums.sha256. Network read."""
    url = f"https://github.com/{repo}/releases/download/{tag}/checksums.sha256"
    body = _fetch(url).decode("utf-8")
    assets: dict[str, str] = {}
    for line in body.splitlines():
        match = re.match(r"^([0-9a-f]{64})\s+\*?(shipyard[A-Za-z0-9_.-]*)$", line.strip())
        if match:
            assets[match.group(2)] = match.group(1)
    if not assets:
        raise ValueError(f"{url} lists no shipyard assets")
    return assets


def record_shipyard_pin(manifest: Path, tag: str, assets: dict[str, str], installer_sha256: str) -> None:
    """Rewrite [shipyard] version and installer_sha256 and the whole [shipyard.assets] table."""
    if not SHIPYARD_TAG.match(tag):
        raise ValueError(f"--shipyard needs an exact release tag such as v0.269.1, not {tag!r}")
    lines = manifest.read_text(encoding="utf-8").splitlines(keepends=True)
    begin, end = _section_bounds(lines, "shipyard")
    index = next((i for i in range(begin + 1, end) if re.match(r"^version\s*=", lines[i])), None)
    if index is None:
        raise ValueError(f"{manifest}: [shipyard] has no version line")
    lines[index] = f'version = "{tag}"\n'
    i_index = next((i for i in range(begin + 1, end) if re.match(r"^installer_sha256\s*=", lines[i])), None)
    if i_index is None:
        raise ValueError(f"{manifest}: [shipyard] has no installer_sha256 line")
    lines[i_index] = f'installer_sha256 = "{installer_sha256}"\n'
    a_begin, a_end = _section_bounds(lines, "shipyard.assets")
    body = [f'"{name}" = "{sha}"\n' for name, sha in sorted(assets.items())]
    lines[a_begin + 1 : a_end] = body + ["\n"]
    manifest.write_text("".join(lines), encoding="utf-8")


def _sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else "absent"


def update_mise_config(path: Path, data: dict, digest: str, check: bool) -> bool:
    text = path.read_text(encoding="utf-8")
    marker = re.compile(re.escape(BEGIN) + r"\n.*?" + re.escape(END) + r"\n", re.S)
    block = render_mise_block(data, digest)
    if not marker.search(text):
        raise ValueError(f"{path}: generated toolchain block is missing")
    result = marker.sub(lambda _m: block, text, count=1)
    if check and result != text:
        raise ValueError(
            f"{path}: the generated block differs from tools/toolchain/manifest.toml "
            "(stale or hand-edited); fix: python3 tools/toolchain/generate.py"
        )
    if not check:
        path.write_text(result, encoding="utf-8")
    return result != text


def _url_fields(node: object):
    if isinstance(node, dict):
        for k, v in node.items():
            if k in ("url", "url_api") and isinstance(v, str):
                yield k, v
            else:
                yield from _url_fields(v)
    elif isinstance(node, list):
        for item in node:
            yield from _url_fields(item)


def _check_urls(path: Path, label: str, node: object, prefixes: list[str]) -> None:
    for field, url in _url_fields(node):
        if not url.startswith(tuple(prefixes)):
            raise ValueError(
                f"{path}: {label} {field} {url} is outside its backend's repository "
                f"(allowed: {', '.join(prefixes)}); regenerate with generate.py --lock, never edit by hand"
            )


def check_mise_lock(path: Path, data: dict) -> None:
    """Offline proof that mise.toolchain.lock pins exactly the manifest's tools.

    Per tool and per lock platform there must be a checksummed entry from the
    tool's own repository, or a reason in the manifest's `unlocked` table. The
    file's bytes must hash to the manifest's lock_sha256, so even a
    well-formed edit (a swapped checksum) is a reviewed manifest change.
    """
    if not path.exists():
        raise ValueError(f"{path}: missing; fix: python3 tools/toolchain/generate.py --lock --mise <pinned mise>")
    want_digest = data["mise"]["lock_sha256"]
    have_digest = _sha256_file(path)
    if have_digest != want_digest:
        raise ValueError(
            f"{path}: sha256 {have_digest} but tools/toolchain/manifest.toml lock_sha256 is {want_digest}; "
            "regenerate with python3 tools/toolchain/generate.py --lock --mise <pinned mise>, which updates both"
        )
    try:
        lock = tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as exc:
        raise ValueError(f"{path}: unreadable: {exc}") from exc
    known = {"lockfile_version", "tools", "conda-packages"}
    unknown = sorted(set(lock) - known)
    if unknown:
        raise ValueError(f"{path}: unexpected top-level tables: {', '.join(unknown)}")
    tools = lock.get("tools", {})
    specs = mise_tools(data)
    expected = {mise_name(key, spec): key for key, spec in specs.items()}
    extra = sorted(set(tools) - set(expected))
    if extra:
        raise ValueError(f"{path}: tools not in the manifest: {', '.join(extra)}")
    conda_prefixes: list[str] = []
    conda_needed: dict[str, set[str]] = {}
    for name, key in expected.items():
        spec = specs[key]
        entries = tools.get(name)
        if not isinstance(entries, list) or len(entries) != 1:
            raise ValueError(f"{path}: {name} must have exactly one locked version")
        entry = entries[0]
        want = data["toolchain"][key]
        if entry.get("version") != want:
            raise ValueError(f"{path}: {name} is locked at {entry.get('version')!r}, manifest pins {want!r}")
        if entry.get("backend") != spec["backend"]:
            raise ValueError(f"{path}: {name} backend is {entry.get('backend')!r}, expected {spec['backend']!r}")
        _check_urls(path, name, entry, spec["url_prefixes"])
        locked = locked_platforms(data, spec)
        rows = {k.split(".", 1)[1] for k in entry if k.startswith("platforms.")}
        stray = sorted(rows - set(locked))
        if stray:
            raise ValueError(f"{path}: {name} has entries for {', '.join(stray)}, which the manifest lists as unlocked")
        for platform in locked:
            row = entry.get(f"platforms.{platform}", {})
            if not str(row.get("checksum", "")).startswith(("sha256:", "blake3:")) or not row.get("url"):
                raise ValueError(
                    f"{path}: {name}@{want} has no checksum and url for {platform}; lock it with "
                    f"generate.py --lock, or add {platform} to mise.tools.{key}.unlocked with a reason"
                )
            # The download must be the pinned release, not another version in
            # the same repository. url_api names a release asset by numeric id,
            # so only `url` can carry the version.
            versions = set(re.findall(r"(?<![0-9.])[0-9]+\.[0-9]+\.[0-9]+(?![0-9])", row["url"]))
            if versions != {want}:
                raise ValueError(
                    f"{path}: {name} {platform} url {row['url']} is not version {want}; "
                    "regenerate with generate.py --lock, never edit by hand"
                )
            if spec["backend"].startswith("conda:"):
                conda_needed.setdefault(platform, set()).update(row.get("conda_deps", []))
        if spec["backend"].startswith("conda:"):
            conda_prefixes += spec["url_prefixes"]
    packages = lock.get("conda-packages", {})
    if packages and not conda_prefixes:
        raise ValueError(f"{path}: conda-packages present but no conda tool is in the manifest")
    for platform, rows in packages.items():
        for pkg, row in rows.items():
            label = f"conda-packages.{platform}.{pkg}"
            _check_urls(path, label, row, conda_prefixes)
            if not str(row.get("checksum", "")).startswith("sha256:"):
                raise ValueError(f"{path}: {label} has no sha256 checksum")
    for platform, deps in conda_needed.items():
        missing = sorted(d for d in deps if d not in packages.get(platform, {}))
        if missing:
            raise ValueError(f"{path}: conda dependencies without a locked package on {platform}: {', '.join(missing)}")


def require_pinned_mise(mise: Path, data: dict) -> None:
    try:
        # MISE_DISABLE_UPDATE_WARNING: `--version` would otherwise ask GitHub
        # for a newer mise; the pinned binary never queries.
        out = subprocess.run(
            [str(mise), "--version"], capture_output=True, text=True, encoding="utf-8", check=False,
            env=dict(os.environ, MISE_DISABLE_UPDATE_WARNING="1"),
        )
    except OSError as exc:
        raise ValueError(f"cannot run {mise}: {exc}") from exc
    words = out.stdout.split()
    have = words[0] if out.returncode == 0 and words else "unknown"
    if have != data["mise"]["version"]:
        raise ValueError(f"{mise} is mise {have}; the lock must be written by mise {data['mise']['version']}")


def write_mise_lock(pulp_root: Path, data: dict, mise: Path) -> str:
    """Regenerate mise.toolchain.lock with the pinned mise and return its sha256.
    Network reads only.

    mise runs with throwaway data, cache, state and config directories so it
    cannot read or write the host's own mise state, and with the global config
    disabled so a host's ~/.config/mise cannot add tools to the lock.
    """
    require_pinned_mise(mise, data)
    lock = pulp_root / MISE_LOCK
    with tempfile.TemporaryDirectory(prefix="pulp-mise-lock-") as td:
        scratch = Path(td)
        env = {
            "HOME": str(scratch),
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "MISE_DATA_DIR": str(scratch / "data"),
            "MISE_CACHE_DIR": str(scratch / "cache"),
            "MISE_STATE_DIR": str(scratch / "state"),
            "MISE_CONFIG_DIR": str(scratch / "config"),
            "MISE_GLOBAL_CONFIG_FILE": str(scratch / "config/none.toml"),
            "MISE_TRUSTED_CONFIG_PATHS": str(pulp_root),
            "MISE_YES": "1",
            "MISE_DISABLE_UPDATE_WARNING": "1",
            "MISE_ENV": MISE_ENV,
        }
        for key in ("GITHUB_TOKEN", "MISE_GITHUB_TOKEN", "SSL_CERT_FILE"):
            if key in os.environ:
                env[key] = os.environ[key]
        if lock.exists():
            lock.unlink()  # a fresh resolve; `mise lock` otherwise keeps old rows
        # `mise lock` does not apply a tool's `os` selector, so tools are
        # locked in groups that share the same set of platforms.
        groups: dict[tuple[str, ...], list[str]] = {}
        for key, spec in mise_tools(data).items():
            groups.setdefault(tuple(locked_platforms(data, spec)), []).append(mise_name(key, spec))
        for platforms, names in groups.items():
            result = subprocess.run(
                [str(mise), "lock", "--platform", ",".join(platforms), *names],
                cwd=pulp_root, env=env, capture_output=True, text=True, encoding="utf-8", check=False,
            )
            if result.returncode != 0:
                raise ValueError(f"mise lock {' '.join(names)} failed:\n" + result.stderr[-4000:])
    return _sha256_file(lock)


def record_lock_digest(manifest: Path, digest: str) -> None:
    text = manifest.read_text(encoding="utf-8")
    if len(LOCK_DIGEST_RE.findall(text)) != 1:
        raise ValueError(f"{manifest}: needs exactly one lock_sha256 line in [mise]")
    manifest.write_text(LOCK_DIGEST_RE.sub(f'lock_sha256 = "{digest}"', text), encoding="utf-8")


def generate(
    pulp_root: Path, tartci_root: Path | None, check: bool, mise: Path | None = None,
    shipyard: str | None = None,
) -> list[Path]:
    manifest = pulp_root / "tools/toolchain/manifest.toml"
    data, digest = load(manifest)
    if shipyard is not None and not check:
        current = data["shipyard"]["version"]
        if SHIPYARD_TAG.match(shipyard) and _tag_key(shipyard) < _tag_key(current):
            raise ValueError(
                f"--shipyard {shipyard} is older than the pinned {current}; a downgrade is a deliberate "
                "decision: edit [shipyard] in tools/toolchain/manifest.toml by hand in a reviewed PR"
            )
        repo = data["shipyard"]["repo"]
        record_shipyard_pin(
            manifest, shipyard, fetch_shipyard_assets(repo, shipyard),
            fetch_shipyard_installer_sha256(repo, shipyard),
        )
        data, digest = load(manifest)
    config = pulp_root / MISE_CONFIG
    tools_config = pulp_root / MISE_TOOLS_CONFIG
    lock = pulp_root / MISE_LOCK
    if mise is not None and not check:
        require_pinned_mise(mise, data)
        # mise reads [tools] from the toolchain config, so write the pins before
        # locking; the lock's digest then goes into the reviewed manifest.
        update_tools_config(tools_config, data, digest, False)
        record_lock_digest(manifest, write_mise_lock(pulp_root, data, mise))
        data, digest = load(manifest)
    check_mise_lock(lock, data)
    update_tools_config(tools_config, data, digest, check)
    update_mise_config(config, data, digest, check)
    update_expected(pulp_root / EXPECTED, data, digest, check)
    update_shipyard_sums(pulp_root / SHIPYARD_SUMS, data, digest, check)
    update_shipyard_pin(pulp_root / SHIPYARD_PIN, data, check)
    targets: list[tuple[Path, str]] = [
        (pulp_root / ".shipyard/vm-image.toml", "macos"),
        (pulp_root / ".shipyard/vm-image.intel.toml", "intel"),
    ]
    if tartci_root:
        targets.extend(
            [
                (tartci_root / "manifests/pulp.macos.toml", "macos"),
                (tartci_root / "manifests/pulp.linux.toml", "linux"),
                (tartci_root / "manifests/pulp.windows.toml", "windows"),
            ]
        )
    if (pulp_root / MISE_IMAGE).exists():
        update_mise_image(pulp_root / MISE_IMAGE, data, digest, check)
    for path, platform in targets:
        update_file(path, data, digest, platform, check)
    return [path for path, _ in targets]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pulp-root", type=Path, default=ROOT)
    parser.add_argument("--tartci-root", type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument(
        "--lock", action="store_true",
        help="also regenerate mise.toolchain.lock (network reads) with the mise named by --mise",
    )
    parser.add_argument("--mise", type=Path, help="the pinned mise binary (manifest [mise].version)")
    parser.add_argument(
        "--shipyard", metavar="TAG",
        help="move the Shipyard pin to this release tag, taking every asset digest from its checksums.sha256 (network read)",
    )
    args = parser.parse_args(argv)
    if args.shipyard and args.check:
        parser.error("--shipyard writes; it cannot be combined with --check")
    if args.lock and args.check:
        parser.error("--lock writes; it cannot be combined with --check")
    if args.lock and args.mise is None:
        parser.error("--lock needs --mise <path to the pinned mise binary>")
    try:
        generate(args.pulp_root, args.tartci_root, args.check, args.mise if args.lock else None, args.shipyard)
    except (OSError, ValueError) as exc:
        print(f"toolchain-generate: ERROR: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
