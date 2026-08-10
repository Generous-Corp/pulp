#!/usr/bin/env python3
"""A patch project: the patch, how it was built, and what it was built from.

A patch on its own is not enough to come back to. Reopening one should restore
the settings it was made under, show what has changed if the user edited it in
Rack, and let a later turn build on the same assumptions rather than the
current defaults. So a project is a directory:

    my-patch/
      patch.vcv          the patch itself, exactly as Rack reads it
      project.json       settings, provenance, and a hash of the last patch
                         we wrote
      revisions/         every version we generated, oldest first

Three settings belong to the patch rather than the app, because the right
answer changes per patch:

  prefer_ours       bias toward the user's own generated modules, or let the
                    whole installed library compete evenly. Sometimes a
                    vendor's oscillator simply fits better.
  create_modules    whether a patch turn may generate a module it lacks. Off
                    by default: patch building is seconds and module building
                    is a minute-plus compile, and being surprised by the
                    latter is worse than being told the library is missing
                    something.
  depth             how much the explanation says: terse, standard, learning.

The hash is what makes reconciliation possible. Rack rewrites a patch on every
save -- ids reorder, positions shift a pixel -- so "has the file changed" can
only be answered against what we last wrote, not against a diff of the text.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys

DEFAULTS = {
    "prefer_ours": True,
    "create_modules": False,
    "depth": "standard",
}
DEPTHS = ("terse", "standard", "learning")


def _hash(patch: dict) -> str:
    """A hash of what the patch *is*, not of how it was written out.

    Keyed on structure only -- which modules, which cables -- so that Rack
    reordering ids or nudging a module by a pixel does not read as an edit.
    Positions and params are deliberately excluded: a moved module is not a
    changed patch, and a turned knob is reported by the diff rather than by
    this.
    """
    mods = sorted((m.get("plugin", ""), m.get("model", ""), m.get("id", 0))
                  for m in patch.get("modules", []))
    cables = sorted((c.get("outputModuleId"), c.get("outputId"),
                     c.get("inputModuleId"), c.get("inputId"))
                    for c in patch.get("cables", []))
    blob = json.dumps({"m": mods, "c": cables}, sort_keys=True)
    return hashlib.sha256(blob.encode()).hexdigest()[:16]


class Project:
    def __init__(self, path: str):
        self.path = path
        self.meta_path = os.path.join(path, "project.json")
        self.patch_path = os.path.join(path, "patch.vcv")
        self.revisions = os.path.join(path, "revisions")
        self.meta = self._load()

    def _load(self) -> dict:
        if os.path.exists(self.meta_path):
            try:
                d = json.load(open(self.meta_path))
            except Exception:
                d = {}
        else:
            d = {}
        # Merge over defaults so a project written by an older version gains
        # new settings rather than losing them.
        d.setdefault("settings", {})
        for k, v in DEFAULTS.items():
            d["settings"].setdefault(k, v)
        d.setdefault("prompts", [])
        d.setdefault("last_written_hash", None)
        return d

    # ── settings ────────────────────────────────────────────────────────────

    def get(self, key: str):
        return self.meta["settings"].get(key, DEFAULTS.get(key))

    def set(self, key: str, value) -> None:
        if key not in DEFAULTS:
            raise KeyError(f"unknown setting {key!r}; known: {sorted(DEFAULTS)}")
        if key == "depth" and value not in DEPTHS:
            raise ValueError(f"depth must be one of {DEPTHS}, got {value!r}")
        if key in ("prefer_ours", "create_modules") and not isinstance(value, bool):
            raise ValueError(f"{key} is a boolean, got {value!r}")
        self.meta["settings"][key] = value

    # ── the patch ───────────────────────────────────────────────────────────

    def save(self, patch: dict, prompt: str | None = None) -> int:
        """Write a new revision and become the current patch. Returns its number."""
        os.makedirs(self.revisions, exist_ok=True)
        n = len([f for f in os.listdir(self.revisions) if f.endswith(".vcv")]) + 1
        json.dump(patch, open(os.path.join(self.revisions, f"{n:03d}.vcv"), "w"),
                  indent=1)
        json.dump(patch, open(self.patch_path, "w"), indent=1)
        if prompt:
            self.meta["prompts"].append({"revision": n, "prompt": prompt})
        self.meta["last_written_hash"] = _hash(patch)
        self.flush()
        return n

    def current(self) -> dict | None:
        if not os.path.exists(self.patch_path):
            return None
        try:
            return json.load(open(self.patch_path))
        except Exception:
            return None

    def edited_elsewhere(self) -> bool:
        """Has the patch changed since we last wrote it?

        False for a patch Rack merely re-saved, true for one a person actually
        rewired. Answering this against our own last hash rather than a text
        diff is the only way to tell those apart.
        """
        p = self.current()
        if p is None or self.meta["last_written_hash"] is None:
            return False
        return _hash(p) != self.meta["last_written_hash"]

    def flush(self) -> None:
        os.makedirs(self.path, exist_ok=True)
        json.dump(self.meta, open(self.meta_path, "w"), indent=2)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    proj = Project(argv[1])
    if len(argv) >= 4 and argv[2] == "set":
        k, v = argv[3].split("=", 1)
        val = {"true": True, "false": False}.get(v.lower(), v)
        proj.set(k, val)
        proj.flush()
    print(f"project {proj.path}")
    for k in sorted(DEFAULTS):
        print(f"  {k:15} {proj.get(k)}")
    print(f"  revisions       {len(proj.meta['prompts'])}")
    print(f"  edited in Rack  {proj.edited_elsewhere()}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
