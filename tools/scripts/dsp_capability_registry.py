#!/usr/bin/env python3
"""Canonical DSP capability registry for Pulp's Forge bake catalogs.

WHY THIS EXISTS
---------------
Until now there was no machine-readable enumeration of the DSP capability
surface anywhere in the tree. Forge hand-maintains one (its effect node
registry), the test suites hand-maintain their own `kAll*` arrays, and nothing
reconciled them. They drifted: Pulp's modulation nodes grew 21 runtime controls
that Forge's registry never bound a name to, so the DSP was reachable from the
catalog and unreachable from generation. That was found by a Forge-side contract
test, i.e. one repository downstream and one merge late.

This script makes the surface explicit ON THE PULP SIDE, where the catalogs
live, so drift is visible at its source.

WHAT IT IS, HONESTLY
--------------------
A STATIC extraction from the catalog headers, not a runtime reflection of
constructed nodes. It reads `k*TypeId` constants and `baked_params.push_back`
declarations out of `core/host/include/pulp/host/forge_*catalog*.hpp`.

That boundary is deliberate and has a consequence worth stating plainly: values
computed at construction time — notably a node's intrinsic latency, which since
the Round 2 integration is a `std::function<int(double)>` evaluated at the
graph's sample rate — CANNOT appear here. A static reader cannot call a
callback. Latency is therefore out of scope for this registry, and the
`latency` key is deliberately absent rather than present-and-wrong.

What it does cover is the part that actually drifted: which nodes exist, what
each advertises as an injectable baked parameter, and the plain-domain range and
default of each.

CHECKS
------
- collision: no two catalog headers may declare the same type_id STRING.
- collision: no node may declare the same baked-param id twice.
- freshness: the committed snapshot must equal a fresh extraction.

The freshness check is the anti-drift mechanism. Adding a baked param to a
catalog without regenerating fails it, which is the failure the Forge-side
contract test caught too late.

USAGE
-----
    dsp_capability_registry.py --json          # emit to stdout
    dsp_capability_registry.py --check         # validate + compare snapshot
    dsp_capability_registry.py --write         # regenerate the snapshot
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re
import sys

SNAPSHOT = pathlib.Path("docs/status/dsp-capabilities.json")

CATALOG_GLOBS = (
    "core/host/include/pulp/host/forge_*catalog*.hpp",
    "core/host/include/pulp/host/detail/forge_*catalog*.hpp",
)

# `k<Name>TypeId = "<value>"`, however the constant is spelled.
TYPE_ID_RE = re.compile(r'(k[A-Za-z0-9_]*TypeId)\s*=\s*"([^"]+)"')
FACTORY_RE = re.compile(r"^inline CustomNodeType (make_[a-z0-9_]+)\(", re.M)
# Enclosing-scope detection must consider EVERY inline definition, not just the
# factories. Several catalogs declare their baked params inside a helper
# (`declare_params(CustomNodeType&, Mode)`) that sits above the factory, so a
# "nearest factory above" heuristic silently attributes those params to the
# PREVIOUS factory — or drops them, making a twelve-param node read as empty.
ANY_FN_RE = re.compile(r"^inline [\w:<>,\s&*]+?\b([a-z_][\w]*)\s*\(", re.M)

# The catalogs declare baked params three different ways, and a reader that
# knows only the first silently reports zero params for nodes that have twelve.
#
#   1. `baked_params.push_back({id, min, max, default})`
#   2. `baked_params = {{id, min, max, default}, {...}}`
#   3. `baked_params.push_back({param_id_for(spec), ...})` inside a LOOP over a
#      spec table — the ids do not exist as literals anywhere in the source.
#
# (1) and (2) are enumerable. (3) is NOT, and the registry says so per node
# rather than emitting an empty list that reads as "this node has no controls".
BAKED_PUSH_RE = re.compile(r"baked_params\.push_back\(\s*\{(.*?)\}\s*\)", re.S)
BAKED_ASSIGN_RE = re.compile(r"baked_params\s*=\s*\{(.*?)\}\s*;", re.S)
# A statically resolvable param id is a `k…` constant. Anything else — a call,
# a loop variable — means the declaration is computed.
STATIC_ID_RE = re.compile(r"^(?:[A-Za-z_][\w]*::)*k[A-Za-z0-9_]+$")


def repo_root() -> pathlib.Path:
    here = pathlib.Path(__file__).resolve()
    for parent in here.parents:
        if (parent / "CMakeLists.txt").exists() and (parent / "core").is_dir():
            return parent
    raise SystemExit("dsp-capabilities: could not locate the repository root")


def split_top_level(body: str) -> list[str]:
    """Split a declaration body on commas that are not inside parentheses."""
    parts, depth, cur = [], 0, ""
    for ch in body:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    parts.append(cur.strip())
    return parts


def catalog_files(root: pathlib.Path) -> list[pathlib.Path]:
    seen: list[pathlib.Path] = []
    for pattern in CATALOG_GLOBS:
        seen.extend(sorted(root.glob(pattern)))
    return seen


def extract(root: pathlib.Path) -> dict:
    catalogs = []
    for path in catalog_files(root):
        src = path.read_text()
        rel = path.relative_to(root).as_posix()

        type_ids = [
            {"constant": m.group(1), "value": m.group(2)}
            for m in TYPE_ID_RE.finditer(src)
        ]

        # Associate each baked-param declaration with its enclosing factory by
        # line span: a declaration belongs to the last factory opened above it.
        factories = [(m.start(), m.group(1)) for m in FACTORY_RE.finditer(src)]
        nodes: dict[str, list] = collections.OrderedDict(
            (name, []) for _, name in factories
        )
        computed: set[str] = set()
        factory_names = {name for _, name in factories}
        scopes = [(m.start(), m.group(1)) for m in ANY_FN_RE.finditer(src)]
        for start, name in factories:
            if not any(s == start for s, _ in scopes):
                scopes.append((start, name))
        scopes.sort()
        helpers: dict[str, list] = collections.OrderedDict()

        def owner_of(pos: int) -> str | None:
            found = None
            for start, name in scopes:
                if start < pos:
                    found = name
                else:
                    break
            return found

        def record(owner: str, body: str) -> None:
            bucket = nodes if owner in factory_names else helpers.setdefault(owner, [])
            fields = split_top_level(" ".join(body.split()))
            if len(fields) != 4:
                return
            if not STATIC_ID_RE.match(fields[0]):
                computed.add(owner)
                return
            entry = {
                "id": fields[0],
                "min": fields[1],
                "max": fields[2],
                "default": fields[3],
            }
            if owner in factory_names:
                nodes[owner].append(entry)
            else:
                bucket.append(entry)

        for m in BAKED_PUSH_RE.finditer(src):
            owner = owner_of(m.start())
            if owner is not None:
                record(owner, m.group(1))

        for m in BAKED_ASSIGN_RE.finditer(src):
            owner = owner_of(m.start())
            if owner is None:
                continue
            # Split the initializer list into its `{...}` elements.
            for element in re.finditer(r"\{([^{}]*)\}", m.group(1)):
                record(owner, element.group(1))

        catalog_nodes = []
        for name, params in nodes.items():
            node = {"factory": name, "baked_params": params}
            if name in computed:
                # Stated explicitly. An empty list here would read as "no
                # controls", which is the opposite of the truth.
                node["baked_params_computed_at_runtime"] = True
                node["note"] = (
                    "This node builds its baked params from a spec table at "
                    "construction time, so their ids do not exist as literals "
                    "in the header and cannot be enumerated statically."
                )
            catalog_nodes.append(node)

        catalog = {"header": rel, "type_ids": type_ids, "nodes": catalog_nodes}
        if helpers or (computed - factory_names):
            # Params declared in a shared helper belong to whichever factories
            # call it. Recorded under the helper rather than guessed at, so the
            # count is never silently attributed to the wrong node.
            catalog["shared_param_declarers"] = [
                {
                    "function": name,
                    "baked_params": params,
                    **(
                        {
                            "baked_params_computed_at_runtime": True,
                            "note": (
                                "Builds its params from a spec table at "
                                "construction time; ids are not literals."
                            ),
                        }
                        if name in computed
                        else {}
                    ),
                }
                for name, params in helpers.items()
            ]
            for name in sorted(computed - factory_names - set(helpers)):
                catalog["shared_param_declarers"].append(
                    {
                        "function": name,
                        "baked_params": [],
                        "baked_params_computed_at_runtime": True,
                    }
                )
        catalogs.append(catalog)

    total_params = sum(
        len(n["baked_params"]) for c in catalogs for n in c["nodes"]
    )
    return {
        "_doc": (
            "Canonical DSP capability surface, extracted statically from the "
            "Forge bake catalog headers by tools/scripts/dsp_capability_registry.py. "
            "Regenerate with `pulp dsp capabilities --write`. Construction-time "
            "values (notably rate-dependent latency) are deliberately absent — a "
            "static reader cannot evaluate them."
        ),
        "counts": {
            "catalogs": len(catalogs),
            "type_ids": sum(len(c["type_ids"]) for c in catalogs),
            "factories": sum(len(c["nodes"]) for c in catalogs),
            "baked_params": total_params,
        },
        "catalogs": catalogs,
    }


def collisions(registry: dict) -> list[str]:
    problems: list[str] = []

    by_value: dict[str, list[str]] = collections.defaultdict(list)
    for cat in registry["catalogs"]:
        for entry in cat["type_ids"]:
            by_value[entry["value"]].append(f"{entry['constant']} in {cat['header']}")
    for value, where in sorted(by_value.items()):
        if len(where) > 1:
            problems.append(
                f"duplicate type_id {value!r} declared by: " + ", ".join(where)
            )

    for cat in registry["catalogs"]:
        for node in cat["nodes"]:
            seen: dict[str, int] = collections.Counter(
                p["id"] for p in node["baked_params"]
            )
            for pid, count in sorted(seen.items()):
                if count > 1:
                    problems.append(
                        f"{cat['header']}:{node['factory']} declares baked param "
                        f"{pid} {count} times"
                    )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description="Pulp DSP capability registry")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--json", action="store_true", help="emit the registry")
    mode.add_argument("--check", action="store_true", help="validate + freshness")
    mode.add_argument("--write", action="store_true", help="regenerate the snapshot")
    args = ap.parse_args()

    root = repo_root()
    registry = extract(root)
    rendered = json.dumps(registry, indent=2, sort_keys=False) + "\n"
    snapshot = root / SNAPSHOT

    if args.json:
        sys.stdout.write(rendered)
        return 0

    if args.write:
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        snapshot.write_text(rendered)
        c = registry["counts"]
        print(
            f"dsp-capabilities: wrote {SNAPSHOT} — {c['catalogs']} catalogs, "
            f"{c['type_ids']} type ids, {c['factories']} factories, "
            f"{c['baked_params']} baked params"
        )
        return 0

    if args.check:
        failed = False
        problems = collisions(registry)
        for problem in problems:
            print(f"dsp-capabilities: COLLISION: {problem}", file=sys.stderr)
            failed = True
        if not problems:
            c = registry["counts"]
            print(
                f"dsp-capabilities: no collisions — {c['type_ids']} type ids "
                f"unique, {c['baked_params']} baked params unique per node"
            )

        if not snapshot.exists():
            print(
                f"dsp-capabilities: STALE: {SNAPSHOT} does not exist. "
                f"Run `pulp dsp capabilities --write`.",
                file=sys.stderr,
            )
            return 1
        if snapshot.read_text() != rendered:
            print(
                f"dsp-capabilities: STALE: {SNAPSHOT} does not match the catalog "
                f"headers. A capability was added or changed without "
                f"regenerating. Run `pulp dsp capabilities --write`.",
                file=sys.stderr,
            )
            failed = True
        else:
            print(f"dsp-capabilities: {SNAPSHOT} is fresh")
        return 1 if failed else 0

    # Default: human summary.
    c = registry["counts"]
    print("Pulp DSP capability registry")
    print(f"  catalogs      {c['catalogs']}")
    print(f"  type ids      {c['type_ids']}")
    print(f"  factories     {c['factories']}")
    print(f"  baked params  {c['baked_params']}")
    print()
    for cat in registry["catalogs"]:
        params = sum(len(n["baked_params"]) for n in cat["nodes"])
        print(
            f"  {pathlib.Path(cat['header']).name:<52} "
            f"{len(cat['type_ids']):>3} ids  {len(cat['nodes']):>2} nodes  "
            f"{params:>3} params"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
