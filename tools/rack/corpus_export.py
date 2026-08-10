#!/usr/bin/env python3
"""Build the keepable corpus: a derived layer that is ours, and bodies that

    corpus_export.py <destination-dir>

may or may not be, depending on what each author's licence permits.

TWO LAYERS, because they carry different permissions.

  derived/   Ours unambiguously, for every patch, whatever its licence. The
             patch id and source URL, when it was fetched, the structure we
             extracted -- which module is which, and which jack connects to
             which -- and our gate's verdict with the requirement that
             rejected it. Facts about a work are not the work. This is the
             part we actually re-use, it is small, and it makes the corpus
             regenerable from the URLs alone.

  bodies/    The patch files themselves, and ONLY where the author's licence
             permits redistribution. A private repository is still a copy.
             For anything unlicensed, custom or non-permissive we keep the
             derived row and the URL and do not carry the file; it can still
             be analysed locally, because reading is not redistributing.

Every row records its licence slug either way, so the decision is auditable
and reversible: a licence read wrongly can be corrected by dropping exactly
the affected rows rather than refetching the world.
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import corpus_sweep as cs                                   # noqa: E402
import idiom_check as ic                                    # noqa: E402
import patch_corpus as pc                                   # noqa: E402


def structure_of(patch: dict) -> dict:
    """What the patch IS, with the file's own noise left behind.

    Module ids in a `.vcv` are 64-bit values unique to one save; carrying them
    would make two extractions of the same patch compare unequal for no reason.
    Cables are recorded against the module's INDEX in this list instead, which
    is stable for a given file and readable by a person.
    """
    mods = patch.get("modules") or []
    index = {m.get("id"): i for i, m in enumerate(mods)}
    return {
        "modules": [{"plugin": m.get("plugin"), "model": m.get("model")}
                    for m in mods],
        "cables": [
            {"from": [index[c["outputModuleId"]], c.get("outputId")],
             "to": [index[c["inputModuleId"]], c.get("inputId")]}
            for c in (patch.get("cables") or [])
            if c.get("outputModuleId") in index and c.get("inputModuleId") in index
        ],
    }


def verdict_of(patch: dict, meta: dict, inv: dict, roles: dict,
               idioms: dict) -> dict:
    """Our gate's judgement, with the reason it could not judge when it could not.

    Coverage travels with the verdict rather than beside it, because a
    rejection from a patch we half understand is not the same claim as one
    from a patch we fully do, and a table that mixes them is a table that
    cannot be argued with.
    """
    known, total = cs.coverage_of(patch, inv)
    out = {"modules_known": known, "modules_total": total}
    prompt = " ".join([meta.get("title", "")] + (meta.get("tags") or []))
    slug = ic.resolve_exact(prompt, idioms)
    if not slug or slug not in idioms:
        out["idiom"] = None
        out["note"] = "no idiom implied by the patch's own title and tags"
        return out
    unchecked: list = []
    problems = ic.check(patch, inv, idioms[slug], roles, unchecked)
    out["idiom"] = slug
    out["rejected_by"] = problems
    out["unverifiable"] = unchecked
    return out


def export(dest: str) -> int:
    os.makedirs(os.path.join(dest, "derived", "structure"), exist_ok=True)
    bodies_dir = os.path.join(dest, "bodies")
    os.makedirs(bodies_dir, exist_ok=True)

    import patch as patch_mod                               # noqa: PLC0415
    inv = patch_mod.inventory()
    roles = ic.load_roles()
    idioms = ic.load_idioms()

    rows = []
    carried = withheld = 0
    for meta, patch in cs.held_patches():
        pid = str(meta.get("id"))
        allowed = pc.may_store_body(meta.get("license_slug", ""))
        row = {
            "id": meta.get("id"),
            "url": meta.get("url"),
            "title": meta.get("title"),
            "author": meta.get("author"),
            "license": meta.get("license"),
            "license_slug": meta.get("license_slug"),
            "fetched_at": meta.get("fetched_at"),
            "sha256": meta.get("sha256"),
            "body_stored": bool(allowed),
            "structure": f"derived/structure/{pid}.json",
            "verdict": verdict_of(patch, meta, inv, roles, idioms),
        }
        rows.append(row)

        with open(os.path.join(dest, "derived", "structure", f"{pid}.json"),
                  "w") as f:
            json.dump(structure_of(patch), f, indent=1, sort_keys=True)

        src = os.path.join(pc.PATCH_DIR, meta["file"])
        if allowed and os.path.exists(src):
            shutil.copy2(src, os.path.join(bodies_dir, f"{pid}.vcv"))
            carried += 1
        else:
            withheld += 1

    rows.sort(key=lambda r: r["id"] or 0)
    manifest = {
        "source": "patchstorage.com",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "patches": len(rows),
        "bodies_carried": carried,
        "bodies_withheld_by_licence": withheld,
        "terms": {
            "terms_of_service": "none published; see README",
            "robots_txt": "User-agent: * / Crawl-delay: 10, no Disallow",
            "crawl_delay_honoured_seconds": pc.CRAWL_DELAY,
            "body_storage_rule": "author's per-patch licence, allowlist in "
                                 "patch_corpus.REDISTRIBUTABLE",
        },
    }
    with open(os.path.join(dest, "derived", "index.json"), "w") as f:
        json.dump({"manifest": manifest, "patches": rows}, f, indent=1,
                  sort_keys=True)
    with open(os.path.join(dest, "MANIFEST.json"), "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)

    print(f"exported {len(rows)} patches to {dest}")
    print(f"  bodies carried:  {carried}")
    print(f"  bodies withheld: {withheld}  (licence does not permit "
          f"redistribution)")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.strip().split("\n\n")[0])
        return 2
    return export(os.path.abspath(argv[1]))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
