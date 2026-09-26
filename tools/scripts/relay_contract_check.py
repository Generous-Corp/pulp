#!/usr/bin/env python3
"""Check that every host the protected macOS gate downloads from is relayed.

The self-hosted macOS gate VMs reach the internet only through an egress relay
whose allowlist tartci publishes as
``profiles/pulp-protected-macos-bootstrap-hosts.toml`` (``literal_hosts`` plus
``transitive_hosts``). A download from any other host fails inside the VM, and
it fails for every gate job at once: a ``pip install`` added to ``build.yml``
before the relay admitted PyPI failed every m5 gate job for a day.

Two checks, both offline against checked-in files:

``gate`` (default; the ``relay-contract-hosts`` ctest)
    Derives the hosts the gate needs and fails, naming each host and the
    tartci file to update, when one is missing from the checked-in copy
    ``tools/scripts/relay_contract_hosts.toml``. Sources:

    * literal ``http(s)://host`` URLs in ``run:`` scripts of ``build.yml``
      jobs that can run on self-hosted macOS (their ``runs-on`` names macOS,
      ``self-hosted`` or a matrix), skipping steps whose ``if:`` confines
      them to Linux or Windows;
    * package managers those scripts invoke, mapped to the hosts they reach
      (``PACKAGE_MANAGER_HOSTS``);
    * ``CORPUS_HOSTS``: downloads made by registered tests rather than by the
      workflow. Each names the file and text that performs it, and a
      declaration whose text disappeared fails too, so the list cannot rot
      into a claim nothing makes.

``--check`` / ``--write`` (the hourly runner-topology workflow)
    Compares the checked-in copy with tartci's current file, from a checkout
    (``--tartci DIR``) or ``--source-url``, and exits 1 naming every host
    added or removed; ``--write`` refreshes the copy. Exit 2 means the source
    could not be read.

    python3 tools/scripts/relay_contract_check.py
    python3 tools/scripts/relay_contract_check.py --tartci ../tartci --check
    python3 tools/scripts/relay_contract_check.py --tartci ../tartci --write
"""

from __future__ import annotations

import argparse
import re
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
DEFAULT_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
DEFAULT_CONTRACT = HERE / "relay_contract_hosts.toml"
TARTCI_FILE = "profiles/pulp-protected-macos-bootstrap-hosts.toml"
PUBLISHED_URL = ("https://raw.githubusercontent.com/danielraffel/tartci/main/"
                 + TARTCI_FILE)
FIX_HINT = (f"add it to tartci {TARTCI_FILE} (literal_hosts, or transitive_hosts "
            "for a package manager's dependencies), land that, then refresh the "
            "copy with `python3 tools/scripts/relay_contract_check.py --tartci "
            "<tartci checkout> --write`")

# A package manager reaches hosts that never appear as a URL in the script.
PACKAGE_MANAGER_HOSTS: dict[str, tuple[re.Pattern[str], tuple[str, ...]]] = {
    "pip": (re.compile(r"\bpip3?\s+install\b|-m\s+pip\s+install\b"),
            ("pypi.org", "files.pythonhosted.org")),
    "npm": (re.compile(r"\bnpm\s+(?:ci|install|i)\b|\bnpx\s"),
            ("registry.npmjs.org",)),
    "brew": (re.compile(r"\bbrew\s+(?:install|update|upgrade|bundle|reinstall)\b"),
             ("ghcr.io", "formulae.brew.sh")),
}


@dataclass(frozen=True)
class CorpusHost:
    host: str
    source: str
    needle: str
    why: str


# Downloads a registered gate test performs itself. Keep each needle an exact
# substring of its source; the check fails when it is gone.
CORPUS_HOSTS = (
    CorpusHost("registry.npmjs.org", "tools/scripts/bundle_threejs_for_jsc.mjs",
               '["install", "--no-audit", "--no-fund"]',
               "pulp_bundle_threejs_for_jsc_smoke provisions esbuild with npm"),
    CorpusHost("api.vcvrack.com", "tools/rack/library_catalog.py",
               "https://api.vcvrack.com/library/manifests",
               "Rack library manifest read by the acid preflight"),
)

URL_HOST = re.compile(r"\bhttps?://([A-Za-z0-9.-]+\.[A-Za-z]{2,})")
NON_MACOS_IF = re.compile(
    r"runner\.os\s*==\s*'(?:Linux|Windows)'|runner\.os\s*!=\s*'macOS'")
MACOS_RUNS_ON = re.compile(r"macos|self-hosted|matrix\.", re.IGNORECASE)


@dataclass
class Step:
    job: str
    line: int
    condition: str
    run: str


def _indent(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


def macos_steps(text: str) -> list[Step]:
    """Steps of jobs that can run on self-hosted macOS, minus non-macOS steps.

    A line-structured reading of the workflow rather than a YAML parse: the
    required macOS gate hosts do not ship PyYAML, and this layout (jobs at
    indent 2, steps as ``- `` items under ``steps:``) is what the file uses.
    """
    lines = text.splitlines()
    try:
        start = next(i for i, l in enumerate(lines) if l.rstrip() == "jobs:")
    except StopIteration:
        return []
    jobs: list[tuple[str, int, int]] = []
    for i in range(start + 1, len(lines)):
        m = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", lines[i])
        if m:
            jobs.append((m.group(1), i, len(lines)))
    jobs = [(name, s, jobs[k + 1][1] if k + 1 < len(jobs) else len(lines))
            for k, (name, s, _e) in enumerate(jobs)]
    steps: list[Step] = []
    for name, begin, end in jobs:
        body = lines[begin + 1:end]
        runs_on = _block(body, "runs-on:", 4)
        if not MACOS_RUNS_ON.search(runs_on):
            continue
        steps_at = next((i for i, l in enumerate(body) if l.strip() == "steps:"), None)
        if steps_at is None:
            continue
        item_indent = None
        current: list[tuple[int, str]] = []
        for i in range(steps_at + 1, len(body)):
            line = body[i]
            if line.strip() and _indent(line) <= _indent(body[steps_at]) \
                    and not line.lstrip().startswith("#"):
                break
            if line.lstrip().startswith("- ") and (
                    item_indent is None or _indent(line) == item_indent):
                item_indent = _indent(line)
                if current:
                    steps.append(_step(name, begin + 1, current))
                current = []
            current.append((begin + 1 + i, line))
        if current:
            steps.append(_step(name, begin + 1, current))
    return [s for s in steps if not NON_MACOS_IF.search(s.condition)]


def _block(body: list[str], key: str, indent: int) -> str:
    for i, line in enumerate(body):
        if _indent(line) == indent and line.strip().startswith(key):
            out = [line]
            for nxt in body[i + 1:]:
                if nxt.strip() and _indent(nxt) <= indent:
                    break
                out.append(nxt)
            return "\n".join(out)
    return ""


def _step(job: str, _offset: int, lines: list[tuple[int, str]]) -> Step:
    first = lines[0][0] + 1
    text = [l for _n, l in lines]
    condition = ""
    run: list[str] = []
    in_run = False
    base = _indent(text[0]) + 2
    for line in text:
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        key = line.lstrip("- ").strip() if line.lstrip().startswith("- ") else stripped
        if _indent(line) <= base and not line.lstrip().startswith("- ") or \
                line.lstrip().startswith("- "):
            in_run = False
            if key.startswith("if:"):
                condition = key[3:].strip()
            if key.startswith("run:"):
                in_run = True
                run.append(key[4:])
                continue
        if in_run:
            run.append(line)
    return Step(job=job, line=first, condition=condition, run="\n".join(run))


def _shell_lines(script: str) -> list[str]:
    return [l for l in script.splitlines() if not l.strip().startswith("#")]


def required_hosts(workflow_text: str, repo_root: Path = REPO_ROOT
                   ) -> tuple[dict[str, list[str]], list[str]]:
    """({host: [why, ...]}, errors) for everything the gate downloads from."""
    need: dict[str, list[str]] = {}
    errors: list[str] = []
    for step in macos_steps(workflow_text):
        code = "\n".join(_shell_lines(step.run))
        where = f"build.yml job `{step.job}` step at line {step.line}"
        for host in URL_HOST.findall(code):
            need.setdefault(host.lower(), []).append(f"{where} (literal URL)")
        for tool, (pattern, hosts) in PACKAGE_MANAGER_HOSTS.items():
            if pattern.search(code):
                for host in hosts:
                    need.setdefault(host, []).append(f"{where} ({tool})")
    for item in CORPUS_HOSTS:
        path = repo_root / item.source
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            errors.append(f"corpus declaration for {item.host}: {item.source} is missing")
            continue
        if item.needle not in text:
            errors.append(f"corpus declaration for {item.host}: {item.source} no longer "
                          f"contains {item.needle!r}; update or remove CORPUS_HOSTS")
            continue
        need.setdefault(item.host, []).append(f"{item.source} ({item.why})")
    return need, errors


def contract_hosts(text: str) -> set[str]:
    data = tomllib.loads(text)
    hosts: set[str] = set()
    for key in ("literal_hosts", "transitive_hosts"):
        value = data.get(key, [])
        if not isinstance(value, list) or not all(isinstance(h, str) for h in value):
            raise ValueError(f"{key} must be a list of host strings")
        hosts.update(h.lower() for h in value)
    return hosts


def gate(workflow: Path, contract: Path, repo_root: Path = REPO_ROOT) -> int:
    try:
        allowed = contract_hosts(contract.read_text(encoding="utf-8"))
    except (OSError, ValueError, tomllib.TOMLDecodeError) as exc:
        print(f"relay-contract: cannot read {contract}: {exc}", file=sys.stderr)
        return 2
    need, errors = required_hosts(workflow.read_text(encoding="utf-8"), repo_root)
    missing = sorted(h for h in need if h not in allowed)
    for host in missing:
        errors.append(f"{host} is downloaded by the protected macOS gate but is not in "
                      f"the relay contract; {FIX_HINT}. Needed by: "
                      + "; ".join(need[host]))
    for err in errors:
        print(f"relay-contract: {err}", file=sys.stderr)
    if errors:
        return 1
    print(f"relay-contract: ok ({len(need)} required host(s), all relayed: "
          + ", ".join(sorted(need)) + ")")
    return 0


def load_source(tartci: Path | None, url: str | None) -> tuple[str, str]:
    if url:
        import urllib.request
        with urllib.request.urlopen(url, timeout=60) as response:  # noqa: S310
            return response.read().decode("utf-8"), url
    assert tartci is not None
    path = tartci / TARTCI_FILE
    return path.read_text(encoding="utf-8"), str(path)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--workflow", type=Path, default=DEFAULT_WORKFLOW)
    ap.add_argument("--contract", type=Path, default=DEFAULT_CONTRACT)
    ap.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    ap.add_argument("--tartci", type=Path)
    ap.add_argument("--source-url")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                      help="compare the checked-in copy with tartci's file")
    mode.add_argument("--write", action="store_true",
                      help="refresh the checked-in copy from tartci's file")
    args = ap.parse_args(argv)

    if not (args.check or args.write):
        return gate(args.workflow, args.contract, args.repo_root)
    if not (args.tartci or args.source_url):
        print("relay-contract: --check/--write need --tartci DIR or --source-url",
              file=sys.stderr)
        return 2
    try:
        source_text, origin = load_source(args.tartci, args.source_url)
        source = contract_hosts(source_text)
    except (OSError, ValueError, tomllib.TOMLDecodeError) as exc:
        print(f"relay-contract: cannot read tartci contract: {exc}", file=sys.stderr)
        return 2
    if args.write:
        args.contract.write_text(source_text, encoding="utf-8")
        print(f"relay-contract: wrote {args.contract} from {origin}")
        return 0
    try:
        local = contract_hosts(args.contract.read_text(encoding="utf-8"))
    except (OSError, ValueError, tomllib.TOMLDecodeError) as exc:
        print(f"relay-contract: cannot read {args.contract}: {exc}", file=sys.stderr)
        return 2
    added, removed = sorted(source - local), sorted(local - source)
    if not added and not removed:
        print(f"relay-contract: copy matches {origin} ({len(local)} hosts)")
        return 0
    for host in added:
        print(f"relay-contract: tartci relays {host}, the checked-in copy does not")
    for host in removed:
        print(f"relay-contract: the checked-in copy lists {host}, tartci no longer relays it")
    print("relay-contract: refresh with `python3 tools/scripts/relay_contract_check.py "
          "--tartci <tartci checkout> --write` and re-run the gate check")
    return 1


if __name__ == "__main__":
    sys.exit(main())
