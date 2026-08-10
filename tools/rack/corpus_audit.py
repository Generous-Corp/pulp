#!/usr/bin/env python3
"""Ask a corpus of real patches what our port matcher gets wrong.

Patches people built, played and shared are patches that mostly work. So a
jack our matcher cannot classify, on a cable a human deliberately made, is a
candidate defect in the matcher rather than a mistake by the patcher. Five
such defects were found one at a time in a single day, each from whichever run
happened to fail; this asks thousands of real cables the same question at once.

    corpus_audit.py jacks          # jack names our matcher cannot place
    corpus_audit.py coverage       # how much of the corpus we can even judge
    corpus_audit.py cables         # what real patches actually connect
    corpus_audit.py usage-priors   # corroborated hints for unmapped ports

WHY CABLES AND NOT PATCHES. A patch is one datapoint and carries the whole
idiom-resolution machinery on top; a cable is a datapoint about exactly one
question -- can we name this jack -- and there are tens of thousands of them.
`GATE 1 CV` failing `gate_in` is invisible in a per-patch pass rate and
unmissable in a frequency-ranked list of unplaceable jacks.

WHY COVERAGE IS REPORTED FIRST AND LOUDLY. Most shared patches use modules
nobody here has installed, and an unmapped module has no jack names at all --
so every cable touching it is unclassifiable for a reason that is NOT a matcher
defect. Counting those as findings would bury the real ones under our own
missing data. Anything measured here is measured over the mapped part and says
so.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import idiom_check  # noqa: E402
import patch as _patch  # noqa: E402
import patch_corpus  # noqa: E402

CORPUS = os.path.expanduser(
    "~/Library/Application Support/Forge Modular/corpus/patchstorage")

# The port kinds a jack could plausibly be, DERIVED rather than listed: a
# hand-written list drifts from `_roles.json` the moment a kind is added or
# renamed, and the first version of this named a `pitch_out` that has never
# existed. `any_in`/`any_out` are excluded because they accept everything, so
# including them would mean no jack is ever unplaceable -- a check that cannot
# fail.
def kinds_for(roles: dict, suffix: str) -> list:
    return sorted(k for k in roles["ports"]
                  if k.endswith(suffix) and not k.startswith("any_"))


def portmap() -> dict:
    import portmap_seed
    return {(m.get("plugin"), m.get("model")): m
            for m in portmap_seed.entries()}


def read_patch(path: str) -> dict | None:
    """The patch document inside a `.vcv`.

    A Rack 2 `.vcv` is a Zstandard-compressed tar holding `patch.json` and a
    `modules/` directory -- the same container as a `.vcvplugin`, and it looks
    like JSON right up until the first byte. Reading it with `json.load` fails
    on a UnicodeDecodeError, which reads as a corrupt download rather than the
    wrong opener, so every patch in the corpus is skipped and the audit reports
    a confident zero cables.
    """
    import subprocess
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        r = subprocess.run(["tar", "--zstd", "-xf", path, "-C", tmp,
                            "patch.json"], capture_output=True)
        if r.returncode != 0:
            return None
        try:
            with open(os.path.join(tmp, "patch.json")) as f:
                return json.load(f)
        except Exception:                                # noqa: BLE001
            return None


def patches() -> list:
    index = os.path.join(CORPUS, "index.json")
    if not os.path.exists(index):
        print(f"no corpus at {CORPUS} — run patch_corpus.py fetch",
              file=sys.stderr)
        return []
    meta = json.load(open(index)).get("patches", {})
    out, unreadable, quarantined, duplicates = [], 0, 0, 0
    seen_bodies: set[str] = set()
    for pid, row in meta.items():
        # The downloader's index may predate the licence-storage guard. Such a
        # body can remain on disk pending an explicit cleanup decision, but it
        # must not silently influence a derived prior. Unknown licences are
        # quarantine, not permission.
        if not patch_corpus.may_store_body(row.get("license_slug") or ""):
            if row.get("file"):
                quarantined += 1
            continue
        body = row.get("sha256") or ""
        if body and body in seen_bodies:
            duplicates += 1
            continue
        if body:
            seen_bodies.add(body)
        path = os.path.join(CORPUS, "patches", row.get("file") or "")
        if not os.path.exists(path):
            continue
        doc = read_patch(path)
        if doc is None:
            unreadable += 1
            continue
        out.append((row, doc))
    if unreadable:
        # Said out loud: a patch we cannot open is our limit, not a finding
        # about the matcher, and it must not quietly shrink the denominator.
        print(f"  note: {unreadable} patch file(s) could not be read")
    if quarantined:
        print(f"  note: {quarantined} body file(s) have a licence outside the "
              "storage allowlist and were quarantined from analysis")
    if duplicates:
        print(f"  note: {duplicates} byte-identical duplicate body file(s) "
              "were counted once")
    return out


def jack(pm: dict, key: tuple, kind: str, index: int):
    """(name, role) for a jack, or None when the module is unmapped.

    None and "unnamed" are different answers and must not be merged: the first
    is our missing data, the second is a module that really has no name for
    that jack.
    """
    entry = pm.get(key)
    if entry is None:
        return None
    ports = entry.get("outputs" if kind == "out" else "inputs") or []
    for p in ports:
        if p.get("index") == index:
            # The role is INFERRED from the jack name, exactly as the live
            # inventory does it -- a port map records names, not roles. Asking
            # the matcher with a null role reports the most canonical jacks in
            # Rack as unplaceable: "1V/octave pitch" fails pitch_in, "Audio"
            # fails audio_in. That is this audit skipping a step, not a defect.
            role = p.get("role")
            if role is None:
                role = _patch.infer_port_role(p.get("name"),
                                              entry.get("tags"), kind)
            return (p.get("name"), role)
    return ("", None)


def placeable(name, role, kinds, roles) -> list:
    return [k for k in kinds
            if idiom_check._port_matches(k, role, name, roles)]


def walk(pm: dict, roles: dict):
    out_kinds = kinds_for(roles, "_out")
    in_kinds = kinds_for(roles, "_in")
    """Every cable in the corpus, with what we could make of its two ends."""
    for meta, doc in patches():
        mods = {m.get("id"): (m.get("plugin"), m.get("model"))
                for m in doc.get("modules", []) if isinstance(m, dict)}
        known = sum(1 for k in mods.values() if k in pm)
        coverage = known / len(mods) if mods else 0.0
        for c in doc.get("cables", []):
            if not isinstance(c, dict):
                continue
            src = mods.get(c.get("outputModuleId"))
            dst = mods.get(c.get("inputModuleId"))
            if not src or not dst:
                continue
            s = jack(pm, src, "out", c.get("outputId"))
            d = jack(pm, dst, "in", c.get("inputId"))
            yield {
                "patch": meta.get("id"), "coverage": coverage,
                "patch_sha256": meta.get("sha256"),
                "source_author": meta.get("author", ""),
                "source_author_id": meta.get("author_id"),
                "src": src, "dst": dst, "s": s, "d": d,
                "s_index": c.get("outputId"), "d_index": c.get("inputId"),
                "s_kinds": placeable(s[0], s[1], out_kinds, roles) if s else None,
                "d_kinds": placeable(d[0], d[1], in_kinds, roles) if d else None,
            }


def cmd_coverage(rows: list) -> None:
    seen = {}
    for r in rows:
        seen[r["patch"]] = r["coverage"]
    if not seen:
        print("  no cables read")
        return
    full = sum(1 for v in seen.values() if v >= 0.999)
    half = sum(1 for v in seen.values() if v >= 0.5)
    print(f"  patches read            : {len(seen)}")
    print(f"  every module mapped     : {full}")
    print(f"  at least half mapped    : {half}")
    print(f"  mean module coverage    : "
          f"{sum(seen.values())/len(seen)*100:.0f}%")
    print(f"  cables                  : {len(rows)}")
    unmapped = sum(1 for r in rows if r["s"] is None or r["d"] is None)
    print(f"  cables touching an unmapped module: {unmapped} "
          f"({100*unmapped/len(rows):.0f}%) — these say nothing about the "
          f"matcher")


def cmd_jacks(rows: list) -> None:
    """Named jacks, on mapped modules, that no port kind will accept.

    This is the whole point: a real cable lands on it, so somebody meant it to
    carry something, and we cannot say what.
    """
    miss_out = collections.Counter()
    miss_in = collections.Counter()
    judged = 0
    for r in rows:
        if r["s"] is None or r["d"] is None:
            continue
        judged += 1
        name, _ = r["s"]
        if name and not r["s_kinds"]:
            miss_out[f"{r['src'][0]}/{r['src'][1]}  {name}"] += 1
        name, _ = r["d"]
        if name and not r["d_kinds"]:
            miss_in[f"{r['dst'][0]}/{r['dst'][1]}  {name}"] += 1
    print(f"  cables judged (both ends mapped): {judged}")
    print(f"  unplaceable outputs: {sum(miss_out.values())}  "
          f"({100*sum(miss_out.values())/judged:.1f}% of judged)"
          if judged else "")
    print(f"  unplaceable inputs : {sum(miss_in.values())}  "
          f"({100*sum(miss_in.values())/judged:.1f}% of judged)"
          if judged else "")
    # Ranked, because the signal is a COMMON jack name failing. A long tail of
    # odd names is a library being various, not a defect.
    for title, counter in (("OUTPUTS no kind accepts", miss_out),
                           ("INPUTS no kind accepts", miss_in)):
        print(f"\n  {title}, most frequent first:")
        for label, n in counter.most_common(15):
            print(f"    {n:5d}  {label}")
        if not counter:
            print("    (none)")


def cmd_cables(rows: list) -> None:
    """What real patches connect, as kind-to-kind. A sanity check on us."""
    pairs = collections.Counter()
    for r in rows:
        if not r["s_kinds"] or not r["d_kinds"]:
            continue
        pairs[f"{'|'.join(r['s_kinds'])} -> {'|'.join(r['d_kinds'])}"] += 1
    print("  most common connections, as our matcher sees them:")
    for label, n in pairs.most_common(20):
        print(f"    {n:5d}  {label}")


def _signal(name: str, role, direction: str) -> str | None:
    """A narrow semantic fact from one mapped cable end.

    This intentionally does not reuse the permissive idiom matcher. The old
    audit asked whether *any* role accepted a jack and therefore learned that
    nearly everything was CV. A usage prior is admissible only when the known
    end says what the signal is specifically enough to teach us about the
    unknown end.
    """
    import re
    label = (name or "").upper()
    roles = {str(x).lower() for x in (role if isinstance(role, list)
                                      else [role]) if x}
    if re.search(r"(?:^|[^A-Z])(?:1V/?OCT|V/?OCT|PITCH)(?:$|[^A-Z])", label):
        return "pitch"
    if re.search(r"(?:^|[^A-Z])(?:CLOCK|CLK)(?:$|[^A-Z])", label):
        return "clock"
    if re.search(r"(?:^|[^A-Z])(?:GATE|TRIG|TRIGGER)(?:$|[^A-Z])", label):
        return "gate"
    if "pitch" in roles:
        return "pitch"
    if "clock" in roles:
        return "clock"
    if roles & {"gate", "trigger"}:
        return "gate"
    # Audio is broad, but unlike CV it is still a useful routing class. Require
    # the measured role or an unmistakable oscillator/listener label; a bare
    # OUT is not evidence.
    audio_labels = {"AUDIO", "SAW", "SINE", "SIN", "TRI", "TRIANGLE",
                    "SQUARE", "SQR", "PULSE", "PLS", "LP", "HP", "BP"}
    if "audio" in roles or label in audio_labels:
        return "audio"
    return None


def usage_prior_report(rows: list, min_support: int = 3) -> dict:
    """Corroborated, non-authoritative port hints for unscanned modules.

    Evidence is deduplicated per source author and proposed fact. Revisions,
    metadata edits, and multiple uploads from one author therefore cannot
    manufacture corroboration. Anonymous uploads conservatively count as one
    source. Nothing here edits the port map: a real CARTOG scan always wins.
    """
    evidence: dict[tuple, set] = collections.defaultdict(set)
    for row in rows:
        author_id = row.get("source_author_id")
        evidence_key = (f"patchstorage-author:{author_id}"
                        if isinstance(author_id, int) and not isinstance(author_id, bool)
                        and author_id > 0 else "patchstorage-author:unknown")
        if row["s"] is None and row["d"] is not None:
            signal = _signal(row["d"][0], row["d"][1], "in")
            if signal:
                key = (*row["src"], "output", row["s_index"], signal)
                evidence[key].add(evidence_key)
        if row["d"] is None and row["s"] is not None:
            signal = _signal(row["s"][0], row["s"][1], "out")
            if signal:
                key = (*row["dst"], "input", row["d_index"], signal)
                evidence[key].add(evidence_key)

    by_port: dict[tuple, set[str]] = collections.defaultdict(set)
    for plugin, model, direction, index, signal in evidence:
        by_port[(plugin, model, direction, index)].add(signal)

    admitted, quarantine = [], []
    for key, patches_seen in sorted(evidence.items(), key=lambda item:
                                    (-len(item[1]), item[0])):
        plugin, model, direction, index, signal = key
        signals = sorted(by_port[(plugin, model, direction, index)])
        row = {
            "plugin": plugin, "model": model, "direction": direction,
            "index": index, "signal": signal, "support": len(patches_seen),
            "provenance": "inferred", "authority": "usage-prior",
            "must_not_override": "cartography",
        }
        if len(signals) > 1:
            row["reason"] = "conflicting inferred meanings: " + ", ".join(signals)
            quarantine.append(row)
        elif len(patches_seen) < min_support:
            row["reason"] = f"support below admission floor {min_support}"
            quarantine.append(row)
        else:
            admitted.append(row)
    return {
        "schema": "forge.patchstorage_usage_priors.v1",
        "source": "patchstorage.com",
        "policy": "inferred hints only; never override CARTOG or certify quality",
        "minimum_distinct_author_support": min_support,
        "admitted": admitted,
        "quarantine": quarantine,
    }


def cmd_usage_priors(rows: list, min_support: int, json_path: str = "") -> None:
    report = usage_prior_report(rows, min_support)
    print(f"  corroborated priors      : {len(report['admitted'])}")
    print(f"  quarantined observations: {len(report['quarantine'])}")
    for row in report["admitted"][:25]:
        print(f"    {row['support']:5d}  {row['plugin']}/{row['model']} "
              f"{row['direction']}[{row['index']}] -> {row['signal']}")
    if not report["admitted"]:
        print("    (none clear the admission floor)")
    if json_path:
        with open(json_path, "w") as f:
            json.dump(report, f, indent=2, sort_keys=True)
        print(f"  wrote proposal-only report: {json_path}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("cmd", choices=["jacks", "coverage", "cables",
                                    "usage-priors"])
    ap.add_argument("--min-support", type=int, default=3,
                    help="distinct patch bodies required for a usage prior")
    ap.add_argument("--json", default="",
                    help="write the usage-priors proposal report")
    a = ap.parse_args()
    if a.min_support < 1:
        ap.error("--min-support must be at least 1")
    roles = idiom_check.load_roles()
    pm = portmap()
    rows = list(walk(pm, roles))
    if not rows:
        print("  no cables read — is the corpus fetched?")
        return 1
    print(f"corpus: {CORPUS}")
    if a.cmd == "usage-priors":
        cmd_usage_priors(rows, a.min_support, a.json)
    else:
        ({"jacks": cmd_jacks, "coverage": cmd_coverage,
          "cables": cmd_cables}[a.cmd])(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
