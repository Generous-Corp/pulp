#!/usr/bin/env python3
"""Check that every role and tag this fleet names is one OWNERS.md defines.

WHY THIS RUNS HERE

`Generous-Corp/infrastructure-issues` owns the role vocabulary: it is the only
side with a CI gate that fails when a chain member is not an `agent:` role, and
that gate is what enforces "no human is ever in an escalation chain". This side
owns which surface each agent is responsible for. Only the set of NAMES overlaps.

The overlap is what rots. If a role is renamed there, nothing over here notices:
`fleet/AGENTS.md` keeps describing a dead name, and a dead role reads exactly
like a live one. Their CI cannot catch it either -- `pulp-planning` is a private
submodule their workflows cannot read, which is why OWNERS.md says in as many
words that "a cross-check has to run where the consuming file lives". This is
that check.

WHAT IT CHECKS

  1. every tag `watchdog.sh` can file under is in the OWNERS-TABLE allowlist
  2. every `agent:` role named in `fleet/AGENTS.md` is in the OWNERS-ROLES block
  3. every role OWNERS-TABLE routes to is one OWNERS-ROLES defines
  4. every tag routes through exactly ROUTED_ROLES_PER_TAG roles -- the fixed
     table width `watchdog.sh` sizes its escalation ladder from
  5. every label `watchdog.sh` can CREATE matches what their `sync_labels.py`
     declares for it, colour and description both

Check 5 covers a collision that only fires while degraded. `watchdog.sh` creates
a declared label that has gone missing rather than dropping it from the issue,
and their `sync_labels.py` reconciles colour and description on labels that
already exist. Those two are only compatible while the text agrees: if it does
not, the backstop writes our wording, their next `--check` reports it as drift,
and it happens on the recovery path, when a label is already missing and
something is already wrong. Their `declared()` is imported and called, not
transcribed, so this cannot pass against a copy of their wording that they have
since changed.

Check 4 is the one that is easy to skip and expensive to miss. The ladder no
longer mirrors role names, it mirrors a COUNT, and a count is invisible when it
goes wrong: add a fourth fallback column over there and the tail of every chain
is silently never offered a turn. That already happened once with a hardcoded
window, and it cost `agent:platform-generalist` -- the universal last resort --
its turn on every ladder.

VERDICTS (three-valued, same discipline as verify.sh)

  0  ok            every name we use is defined there
  1  mismatch      we name something OWNERS.md does not define
  2  unobservable  OWNERS.md could not be read -- NOT a pass

Exit 2 exists because "I could not check" and "I checked and it was fine" are
different facts, and collapsing them is how a rename ships unnoticed.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys

REPO = "Generous-Corp/infrastructure-issues"
TABLE_BEGIN = "<!-- OWNERS-TABLE:BEGIN -->"
TABLE_END = "<!-- OWNERS-TABLE:END -->"
ROLES_BEGIN = "<!-- OWNERS-ROLES:BEGIN -->"
ROLES_END = "<!-- OWNERS-ROLES:END -->"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
INFRA_REPO_DEFAULT = os.path.join(os.path.dirname(ROOT), "infrastructure-issues")


def fetch_owners(local: str | None) -> str:
    """Return OWNERS.md text, or raise RuntimeError if it cannot be read."""
    if local:
        with open(local, encoding="utf-8") as fh:
            return fh.read()
    exe = shutil.which("ghapp")
    if not exe:
        # `gh` is deliberately not a fallback: it burns the personal token's
        # shared rate limit, and this check runs on a timer.
        raise RuntimeError("ghapp not on PATH")
    try:
        proc = subprocess.run(
            [exe, "api", f"repos/{REPO}/contents/OWNERS.md",
             "-H", "Accept: application/vnd.github.raw"],
            check=True, capture_output=True, text=True, timeout=60)
    except Exception as exc:  # noqa: BLE001 - any failure is "unobservable"
        raise RuntimeError(f"could not fetch OWNERS.md: {exc}") from exc
    return proc.stdout


def _rows(text: str, begin: str, end: str) -> list[list[str]]:
    """Return the data rows of the markdown table between two markers.

    Only the marked region is parsed. OWNERS.md's prose mentions role names
    outside both blocks, and parsing that too would let a role that was merely
    DESCRIBED -- or described as removed -- still look defined.

    A missing marker raises rather than returning empty, matching the failure
    mode their own `parse_roles()` was built with: reading zero roles as "no
    roles defined" turns a rename of the anchors into a silent pass.
    """
    if begin not in text or end not in text:
        raise RuntimeError(f"{begin} / {end} not found in OWNERS.md; format changed")
    body = text.split(begin, 1)[1].split(end, 1)[0]
    rows = []
    for line in body.splitlines():
        line = line.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip().strip("`").strip() for c in line.strip("|").split("|")]
        if not cells or set("".join(cells)) <= {"-", ":", " "}:
            continue  # separator
        if cells[0].lower() in ("tag", "role"):
            continue  # header
        rows.append(cells)
    if not rows:
        raise RuntimeError(f"table between {begin} and {end} parsed empty; format changed")
    return rows


def parse_owners(text: str) -> tuple[dict[str, list[str]], set[str]]:
    """Return ({tag: [owner, *fallbacks]}, {defined roles})."""
    chains = {r[0]: [c for c in r[1:] if c] for r in _rows(text, TABLE_BEGIN, TABLE_END)}
    roles = {r[0] for r in _rows(text, ROLES_BEGIN, ROLES_END) if r[0]}
    return chains, roles


def watchdog_facts(path: str) -> tuple[list[str], int | None]:
    """Return (tags the watchdog can file under, its ladder width).

    Both are read out of the shipped script rather than restated here, so this
    check cannot pass while the watchdog files under a tag it never heard of or
    sizes its ladder to a width nobody verified.
    """
    src = open(path, encoding="utf-8").read()
    block = re.search(r"^TAGS = \{(.*?)^\}", src, re.M | re.S)
    if not block:
        raise RuntimeError("no TAGS block found in watchdog.sh; it may have been renamed")
    tags = re.findall(r'"(infra-[a-z0-9-]+)"', block.group(1))
    width = re.search(r"^ROUTED_ROLES_PER_TAG = (\d+)", src, re.M)
    return tags, int(width.group(1)) if width else None


def watchdog_label_specs(path: str) -> dict[str, tuple[str, str]]:
    """Return the labels `watchdog.sh` is willing to create, as name -> (colour, text).

    Executes the two matched assignments rather than regexing the pairs apart,
    because the values reference `_TAG_DESC` and a regex would have to reproduce
    that indirection -- a second place for the wording to be wrong.
    """
    src = open(path, encoding="utf-8").read()
    block = re.search(r"^_TAG_DESC = .*?^LABEL_SPECS = \{.*?^\}", src, re.M | re.S)
    if not block:
        raise RuntimeError("no LABEL_SPECS block in watchdog.sh; it may have been renamed")
    ns: dict = {}
    exec(compile(block.group(0), path, "exec"), ns)  # noqa: S102 - our own two assignments
    return ns["LABEL_SPECS"]


def declared_labels(repo_dir: str) -> dict[str, tuple[str, str]]:
    """Return their authoritative label declaration by calling their own code.

    Importing beats parsing here: `state:open`'s description is read from their
    README's lifecycle table at run time, so no regex over `sync_labels.py`
    could produce the string they would actually write. Raises on anything that
    stops that call from being made, so the caller can report "could not check"
    instead of banking a pass it never earned.
    """
    scripts = os.path.join(repo_dir, ".github", "scripts")
    if not os.path.isdir(scripts):
        raise RuntimeError(f"{scripts} not found")
    sys.path.insert(0, scripts)
    try:
        import lifecycle  # noqa: PLC0415
        import owners as their_owners  # noqa: PLC0415
        import sync_labels  # noqa: PLC0415

        return sync_labels.declared(
            their_owners.load(), their_owners.load_roles(),
            lifecycle.LIFECYCLE_STATES, sync_labels.state_meanings())
    except Exception as exc:  # their parsers raise by design; do not swallow
        raise RuntimeError(f"could not call sync_labels.declared(): {exc}") from exc
    finally:
        sys.path.remove(scripts)


def agents_roles(path: str) -> list[str]:
    """Return every `agent:...` role name mentioned in fleet/AGENTS.md."""
    if not os.path.exists(path):
        raise RuntimeError(f"{path} not found")
    return sorted(set(re.findall(r"\bagent:[a-z0-9-]+", open(path, encoding="utf-8").read())))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--owners", help="read OWNERS.md from this path instead of the API")
    ap.add_argument("--agents", default=os.path.join(ROOT, "planning/fleet/AGENTS.md"))
    ap.add_argument("--watchdog", default=os.path.join(HERE, "watchdog.sh"))
    # Check 5 needs their CODE, not just their OWNERS.md, so it cannot run off
    # the API path. Defaulting to the sibling checkout means it runs by default
    # on any machine that has both repos -- and where it cannot run, it says so
    # in a line of its own rather than being quietly absent from a green run.
    ap.add_argument("--infra-repo", default=INFRA_REPO_DEFAULT,
                    help="local infrastructure-issues checkout, for the label-spec check")
    args = ap.parse_args()

    try:
        chains, roles = parse_owners(fetch_owners(args.owners))
    except (RuntimeError, OSError) as exc:
        print(f"VERDICT=unobservable  {exc}")
        print("Could not read the roster. This is NOT a pass -- a rename made "
              "while this check was blind looks identical to no rename at all.")
        return 2

    try:
        our_tags, width = watchdog_facts(args.watchdog)
        our_roles = agents_roles(args.agents)
        our_labels = watchdog_label_specs(args.watchdog)
    except (RuntimeError, OSError) as exc:
        print(f"VERDICT=unobservable  could not read local config: {exc}")
        return 2

    # Their declaration, called rather than copied. Absent checkout is a
    # different fact from a failed call: the first is how you invoked this, the
    # second is a check that should have run and did not.
    label_note = None
    their_labels: dict[str, tuple[str, str]] | None = None
    if os.path.isdir(args.infra_repo):
        try:
            their_labels = declared_labels(args.infra_repo)
        except RuntimeError as exc:
            print(f"VERDICT=unobservable  {exc}")
            print("Their label declaration could not be read, so we cannot tell "
                  "whether what we would create still matches it. NOT a pass.")
            return 2
    else:
        label_note = (f"label specs NOT CHECKED: no checkout at {args.infra_repo} "
                      f"(pass --infra-repo); what watchdog.sh would create is unverified")

    problems = []
    for t in our_tags:
        if t not in chains:
            problems.append(f"watchdog can file under `{t}`, not in the OWNERS.md allowlist")
    for r in our_roles:
        if r not in roles:
            problems.append(f"AGENTS.md names role `{r}`, not defined in OWNERS-ROLES")
    for tag, chain in sorted(chains.items()):
        for r in chain:
            if r not in roles:
                problems.append(f"OWNERS-TABLE routes `{tag}` to `{r}`, not in OWNERS-ROLES")
        if width is not None and len(chain) != width:
            problems.append(
                f"`{tag}` routes through {len(chain)} roles but watchdog.sh sizes its "
                f"ladder to {width}; the tail of that chain never gets a turn")
    for name, (colour, desc) in sorted((their_labels or {}).items()):
        if name not in our_labels:
            continue  # they declare 25; we only ever create the handful we file with
        if our_labels[name] != (colour, desc):
            ours = our_labels[name]
            problems.append(
                f"watchdog.sh would create `{name}` as {ours!r} but sync_labels.py "
                f"declares {(colour, desc)!r}; creating it would land as drift there")
    if not our_tags:
        problems.append("no tags found in watchdog.sh; the TAGS block may have changed")
    if not our_labels:
        problems.append("no LABEL_SPECS in watchdog.sh; its label backstop is gone")
    if width is None:
        problems.append("no ROUTED_ROLES_PER_TAG in watchdog.sh; ladder width unverified")

    print(f"OWNERS.md defines {len(chains)} tags, {len(roles)} roles; "
          f"ladder width {width}")
    print(f"AGENTS.md names roles: {', '.join(our_roles) or '(none)'}")
    print(f"watchdog files under: {', '.join(sorted(our_tags)) or '(none)'}")
    if label_note:
        print(label_note)
    else:
        checked = sorted(set(our_labels) & set(their_labels or {}))
        print(f"label specs checked against sync_labels.declared(): "
              f"{', '.join(checked) or '(none matched)'}")
        # A name we would create that they do not declare cannot be compared to
        # anything, so it cannot fail above -- say it out loud rather than let it
        # sit inside a clean run.
        for name in sorted(set(our_labels) - set(their_labels or {})):
            print(f"  note: `{name}` is ours alone; sync_labels.py does not declare it")
    if problems:
        print("VERDICT=mismatch")
        for p in problems:
            print(f"  - {p}")
        return 1
    print("VERDICT=ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
