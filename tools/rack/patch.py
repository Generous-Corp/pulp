#!/usr/bin/env python3
"""Build, explain, check and reconcile VCV Rack patches.

    patch.py inventory                    # what this machine can patch with
    patch.py explain <file.vcv>           # signal flow, in readable form
    patch.py lint    <file.vcv>           # would Rack actually load this?
    patch.py diff    <a.vcv> <b.vcv>      # what changed, structurally

A patch is JSON: modules carry a plugin slug, a model slug, a position and
param values; cables join an output port on one module to an input port on
another. Nothing in that file is Forge-specific, so a patch can wire any
module the user already has, whoever wrote it.

The explanation here is derived entirely from the patch and the local
inventory -- no model is involved. That is deliberate twice over: it costs
nothing to re-render, and an explanation computed from the file cannot claim a
cable that is not in it. Prose about *why* a connection was made is a separate,
much smaller thing layered on top by the generator.
"""
from __future__ import annotations

import json
import os
import sys

RACK_USER = os.path.expanduser("~/Library/Application Support/Rack2")
PLUGIN_DIRS = [os.path.join(RACK_USER, d) for d in os.listdir(RACK_USER)] \
    if os.path.isdir(RACK_USER) else []
PLUGIN_DIRS = [d for d in PLUGIN_DIRS if os.path.basename(d).startswith("plugins-")]

# Our own manifests carry real port names and roles; a third-party plugin.json
# does not describe ports at all, so those stay as indices until something that
# runs inside Rack can read PortInfo.
PACK_MODULES = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "..", "examples", "forge-modular", "modules")


# ── Inventory ────────────────────────────────────────────────────────────────

def _record(inv: dict, d: dict, fallback_slug: str) -> None:
    slug = d.get("slug") or fallback_slug
    inv[slug] = {
        "name": d.get("name", slug),
        "brand": d.get("brand", ""),
        "version": d.get("version", ""),
        "modules": {m["slug"]: {"name": m.get("name", m["slug"]),
                                "description": m.get("description", ""),
                                "tags": m.get("tags", [])}
                    for m in d.get("modules", []) if "slug" in m},
    }


def _read_vcvplugin(path: str) -> dict | None:
    """plugin.json out of a .vcvplugin, which is a zstd-compressed tar.

    Rack unpacks a plugin the first time it loads it, so a freshly installed
    one is still an archive on disk. Skipping those would report a plugin as
    missing right after installing it -- and the lint would then reject the
    very patch that was built to use it.
    """
    import io
    import subprocess
    import tarfile
    try:
        raw = subprocess.run(["zstd", "-dc", path], capture_output=True,
                             timeout=60).stdout
        with tarfile.open(fileobj=io.BytesIO(raw)) as tf:
            for member in tf.getmembers():
                if os.path.basename(member.name) == "plugin.json":
                    f = tf.extractfile(member)
                    return json.load(f) if f else None
    except Exception:
        return None
    return None


def inventory() -> dict:
    """Every module this machine can patch with, keyed plugin -> model."""
    inv: dict = {}

    # Core is compiled into Rack itself rather than installed, so its modules
    # (the audio and MIDI interfaces every patch needs to be heard) appear in
    # no plugin directory. Its manifest ships in the app bundle.
    for core in ("/Applications/VCV Rack 2 Free.app/Contents/Resources/Core.json",
                 "/Applications/VCV Rack 2 Pro.app/Contents/Resources/Core.json"):
        if os.path.exists(core):
            try:
                _record(inv, json.load(open(core)), "Core")
            except Exception:
                pass
            break

    for pdir in PLUGIN_DIRS:
        if not os.path.isdir(pdir):
            continue
        for entry in sorted(os.listdir(pdir)):
            full = os.path.join(pdir, entry)
            man = os.path.join(full, "plugin.json")
            if os.path.exists(man):
                try:
                    _record(inv, json.load(open(man)), entry)
                except Exception:
                    pass
            elif entry.endswith(".vcvplugin"):
                d = _read_vcvplugin(full)
                if d:
                    _record(inv, d, entry.split("-")[0])
    _add_port_names(inv)
    _add_portmap(inv)
    return inv


PORTMAP = os.path.join(RACK_USER, "forge-portmap.json")


def _add_portmap(inv: dict) -> None:
    """Fold in real port names and positions recorded from inside Rack.

    Written by the CARTOG module, which is the only thing that can see them:
    a port's index, name and jack position exist solely in compiled widget
    code. This is what lets a patch be wired to a named port on someone
    else's module instead of to index 0 and a hope.

    It also settles a thing no amount of inference would have: index order is
    not visual order. Fundamental's VCO puts input 1 (frequency modulation) to
    the LEFT of input 0 (1V/octave), so guessing indices from panel layout --
    or labelling badges left-to-right by index -- produces confident nonsense.
    """
    if not os.path.exists(PORTMAP):
        return
    try:
        doc = json.load(open(PORTMAP))
    except Exception:
        return
    for entry in doc.get("modules", []):
        plug = inv.get(entry.get("plugin"))
        if not plug:
            continue
        mod = plug["modules"].get(entry.get("model"))
        if mod is None:
            continue
        for kind, key in (("inputs", "inputs"), ("outputs", "outputs")):
            ports = entry.get(kind) or []
            if not ports:
                continue
            width = max(p["index"] for p in ports) + 1
            names = [None] * width
            coords = [None] * width
            for p in ports:
                names[p["index"]] = p.get("name") or None
                coords[p["index"]] = (p.get("x"), p.get("y"))
            # The map is authoritative: it was read from the running module,
            # whereas anything already here was derived from a manifest.
            mod[key] = names
            mod[key + "_xy"] = coords
        mod["panel"] = entry.get("size")


def _add_port_names(inv: dict, our_plugin: str = "ForgeModular") -> None:
    """Attach real port names for our own modules, from their manifests.

    Scoped to our plugin only. Model slugs are unique within a plugin but not
    across the library -- Fundamental also ships VCO, VCF, VCA and LFO -- so
    matching on the model slug alone labels a vendor's ports with our port
    names. A confidently wrong label is worse than an index here, because the
    whole point of the explanation is that someone can learn from it.
    """
    mdir = os.path.normpath(PACK_MODULES)
    if not os.path.isdir(mdir) or our_plugin not in inv:
        return
    for f in sorted(os.listdir(mdir)):
        if not f.endswith(".json") or f.startswith("_"):
            continue
        try:
            doc = json.load(open(os.path.join(mdir, f)))
        except Exception:
            continue
        for m in doc.get("modules", []):
            for pslug, plug in inv.items():
                if pslug != our_plugin:
                    continue
                mod = plug["modules"].get(m.get("slug"))
                if mod is None:
                    continue
                mod["inputs"] = [p.get("label") or p.get("name")
                                 for p in m.get("inputs", [])]
                mod["outputs"] = [p.get("label") or p.get("name")
                                  for p in m.get("outputs", [])]
                mod["roles_in"] = [p.get("role", "Cv") for p in m.get("inputs", [])]
                mod["roles_out"] = [p.get("role", "Cv") for p in m.get("outputs", [])]


def port_name(inv, plugin, model, kind, idx):
    """A port's name if we know it, else its index. Never a guess."""
    mod = inv.get(plugin, {}).get("modules", {}).get(model, {})
    names = mod.get("outputs" if kind == "out" else "inputs")
    if names and 0 <= idx < len(names) and names[idx]:
        return names[idx]
    return f"{'OUT' if kind == 'out' else 'IN'} {idx}"


def port_role(inv, plugin, model, kind, idx):
    mod = inv.get(plugin, {}).get("modules", {}).get(model, {})
    roles = mod.get("roles_out" if kind == "out" else "roles_in")
    if roles and 0 <= idx < len(roles):
        return roles[idx]
    return None


# ── Explanation ──────────────────────────────────────────────────────────────

# Which heading a cable falls under. Role comes from our manifests when we have
# it; otherwise the module's own tags are a decent proxy, since a module tagged
# "Oscillator" feeding something is almost always carrying audio.
AUDIO_TAGS = {"Oscillator", "Waveshaper", "Filter", "Reverb", "Delay", "Drum",
              "Distortion", "Chorus", "Phaser", "Flanger", "Synth voice",
              "Granular", "Sampler", "Noise", "Ring modulator", "Mixer",
              "Voltage-controlled amplifier", "Low-pass gate", "Equalizer",
              "Compressor", "Limiter", "Vocoder", "Physical modeling"}
MOD_TAGS = {"Low-frequency oscillator", "Envelope generator", "Random",
            "Sample and hold", "Slew limiter", "Function generator",
            "Envelope follower", "Attenuator"}
CLOCK_TAGS = {"Clock generator", "Clock modulator", "Sequencer", "Arpeggiator"}


def _group_of(inv, mod_ref, out_idx, in_role):
    if in_role in ("Pitch", "Gate", "Trigger", "Clock"):
        return {"Pitch": "PITCH & GATE", "Gate": "PITCH & GATE",
                "Trigger": "PITCH & GATE", "Clock": "PITCH & GATE"}[in_role]
    if in_role == "Audio":
        return "AUDIO"
    if in_role == "Cv":
        return "MODULATION"
    tags = set(inv.get(mod_ref[0], {}).get("modules", {})
               .get(mod_ref[1], {}).get("tags", []))
    if tags & CLOCK_TAGS:
        return "PITCH & GATE"
    if tags & MOD_TAGS:
        return "MODULATION"
    if tags & AUDIO_TAGS:
        return "AUDIO"
    return "MODULATION"


def label(inv, m):
    """How a module is referred to in prose: its model name, not its slug."""
    mod = inv.get(m["plugin"], {}).get("modules", {}).get(m["model"])
    return (mod or {}).get("name") or m["model"]


def explain(patch: dict, inv: dict, why: dict | None = None) -> str:
    """The patch's signal flow, grouped by role. Computed, never asserted."""
    by_id = {m["id"]: m for m in patch.get("modules", [])}
    groups: dict[str, list[str]] = {}
    for c in patch.get("cables", []):
        src, dst = by_id.get(c.get("outputModuleId")), by_id.get(c.get("inputModuleId"))
        if not src or not dst:
            continue                       # lint reports these; do not narrate them
        oi, ii = c.get("outputId", 0), c.get("inputId", 0)
        sp = port_name(inv, src["plugin"], src["model"], "out", oi)
        dp = port_name(inv, dst["plugin"], dst["model"], "in", ii)
        role = port_role(inv, dst["plugin"], dst["model"], "in", ii)
        g = _group_of(inv, (src["plugin"], src["model"]), oi, role)
        line = f"  {label(inv, src)} {sp} → {label(inv, dst)} {dp}"
        groups.setdefault(g, []).append(line)
        note = (why or {}).get(f"{c.get('outputModuleId')}:{oi}>"
                              f"{c.get('inputModuleId')}:{ii}")
        if note:
            groups[g].append(f"      {note}")

    out = []
    for g in ("AUDIO", "PITCH & GATE", "MODULATION", "OUTPUT"):
        if g in groups:
            out.append(g)
            out.extend(groups[g])
            out.append("")
    for g, lines in groups.items():        # anything unrecognised, still shown
        if g not in ("AUDIO", "PITCH & GATE", "MODULATION", "OUTPUT"):
            out.append(g)
            out.extend(lines)
            out.append("")
    if not out:
        n = len(patch.get("modules", []))
        return (f"{n} module(s), nothing patched together yet — "
                f"no cables in this file.") if n else "empty patch"
    return "\n".join(out).rstrip()


# ── Lint ─────────────────────────────────────────────────────────────────────

def lint(patch: dict, inv: dict) -> list[str]:
    """Reasons Rack would not load this patch as intended.

    Rack drops a module it cannot resolve and says so only in its log, so a
    patch naming a plugin the user does not have opens as a partially empty
    rack rather than an error. Checking here is what turns that into a message.
    """
    errs: list[str] = []
    mods = patch.get("modules", [])
    ids = [m.get("id") for m in mods]

    if not mods:
        errs.append("patch has no modules")
    if len(set(ids)) != len(ids):
        errs.append("duplicate module ids — Rack keys cables by id")

    for m in mods:
        plug = inv.get(m.get("plugin"))
        if plug is None:
            errs.append(f"plugin '{m.get('plugin')}' is not installed — "
                        f"Rack will silently drop this module")
            continue
        if m.get("model") not in plug["modules"]:
            errs.append(f"'{m['plugin']}' has no model '{m.get('model')}' "
                        f"(has: {', '.join(sorted(plug['modules'])[:6])}…)")
        if not isinstance(m.get("pos"), list) or len(m.get("pos", [])) != 2:
            errs.append(f"module {m.get('id')} has no valid pos [x, y]")

    known = set(ids)
    for c in patch.get("cables", []):
        for end in ("outputModuleId", "inputModuleId"):
            if c.get(end) not in known:
                errs.append(f"cable {c.get('id')} references module "
                            f"{c.get(end)}, which is not in the patch")

    # A patch that reaches no audio interface makes no sound, which is a far
    # more common generated failure than a malformed file.
    sinks = [m for m in mods
             if "Audio" in inv.get(m.get("plugin"), {})
             .get("modules", {}).get(m.get("model"), {}).get("tags", [])
             or m.get("model", "").lower().startswith("audio")]
    if mods and not sinks:
        errs.append("no audio interface module — this patch cannot be heard")
    else:
        fed = {c.get("inputModuleId") for c in patch.get("cables", [])}
        if sinks and not any(s.get("id") in fed for s in sinks):
            errs.append("the audio interface has nothing patched into it — silence")

    return errs


# ── Diff, for reconciliation ─────────────────────────────────────────────────

def diff(old: dict, new: dict, inv: dict) -> list[str]:
    """What a user changed in Rack, structurally rather than textually.

    Rack rewrites the whole file on save -- ids get reordered, positions shift
    by a pixel, params gain entries -- so a textual comparison reports a change
    every single time. Only structure is worth showing a person.
    """
    def mods(p):
        return {(m["plugin"], m["model"], m["id"]): m for m in p.get("modules", [])}

    def cables(p):
        return {(c.get("outputModuleId"), c.get("outputId"),
                 c.get("inputModuleId"), c.get("inputId")) for c in p.get("cables", [])}

    o, n = mods(old), mods(new)
    out = []
    for k in n.keys() - o.keys():
        out.append(f"+ added module   {k[0]}/{k[1]}")
    for k in o.keys() - n.keys():
        out.append(f"- removed module {k[0]}/{k[1]}")

    oc, nc = cables(old), cables(new)
    by_id_new = {m["id"]: m for m in new.get("modules", [])}
    by_id_old = {m["id"]: m for m in old.get("modules", [])}

    def cable_str(c, table):
        s, d = table.get(c[0]), table.get(c[2])
        if not s or not d:
            return f"module {c[0]} port {c[1]} → module {c[2]} port {c[3]}"
        return (f"{label(inv, s)} {port_name(inv, s['plugin'], s['model'], 'out', c[1])}"
                f" → {label(inv, d)} {port_name(inv, d['plugin'], d['model'], 'in', c[3])}")

    for c in nc - oc:
        out.append(f"+ added cable    {cable_str(c, by_id_new)}")
    for c in oc - nc:
        out.append(f"- removed cable  {cable_str(c, by_id_old)}")

    for k in o.keys() & n.keys():
        ov = {p["id"]: p["value"] for p in o[k].get("params", [])}
        nv = {p["id"]: p["value"] for p in n[k].get("params", [])}
        for pid in sorted(set(ov) | set(nv)):
            a, b = ov.get(pid), nv.get(pid)
            if a is not None and b is not None and abs(a - b) > 1e-6:
                out.append(f"~ {k[1]} param {pid}: {a:.3f} → {b:.3f}")
    return out


# ── Library catalog (cached) ─────────────────────────────────────────────────

CACHE_DIR = os.path.expanduser("~/.cache/forge-modular")
CATALOG = os.path.join(CACHE_DIR, "library.json")
CATALOG_URL = "https://api.vcvrack.com/library/manifests?version=2"
CATALOG_MAX_AGE_DAYS = 7


def catalog(refresh: bool = False, max_age_days: int = CATALOG_MAX_AGE_DAYS) -> dict:
    """The whole Rack 2 library, cached on disk.

    One request returns every plugin, so there is no reason to ask more than
    once a week: the catalog is a few hundred KB and changes slowly. Keeping
    it local means name matching and brand filtering are instant and work
    offline, which matters when it is feeding a prompt.

    Note what this does *not* contain: modules. The endpoint carries
    plugin-level metadata only, so it can tell you a brand exists and whether
    it costs money, but not what its modules are called.
    """
    import time
    fresh = False
    if os.path.exists(CATALOG) and not refresh:
        age = time.time() - os.path.getmtime(CATALOG)
        fresh = age < max_age_days * 86400
    if not fresh:
        import urllib.request
        try:
            with urllib.request.urlopen(CATALOG_URL, timeout=30) as r:
                data = json.loads(r.read().decode())
            os.makedirs(CACHE_DIR, exist_ok=True)
            json.dump(data, open(CATALOG, "w"))
        except Exception as e:
            if not os.path.exists(CATALOG):
                raise SystemExit(f"could not fetch the library catalog: {e}")
            print(f"  (catalog refresh failed, using cache: {e})", file=sys.stderr)
    return json.load(open(CATALOG)).get("manifests", {})


def resolve_mention(term: str, cat: dict, inv: dict) -> dict:
    """Turn '@mutable instruments' or '@Vult' into concrete plugins.

    Brands are the level the catalog actually supports, and they are what a
    person says out loud -- "Mutable Instruments modules only" means a maker,
    not a plugin slug. The official Mutable port is published as Audible
    Instruments, so matching has to cover brand, plugin name and slug.
    """
    t = term.lstrip("@").strip().lower()
    if not t:
        return {}
    # Weighted by field, because the same word means different things in
    # different places. "Vult" as a brand is what someone asking for Vult
    # means; "Vult-DSP" as the author of a Synthesizers.com port is a true but
    # unwanted match. And the colloquial name for a maker often lives only in
    # the description -- the official Mutable Instruments port is published as
    # "Audible Instruments" and says so only there.
    WEIGHTS = (("brand", 100), ("name", 90), ("slug", 70),
               ("author", 30), ("description", 20))
    hits = {}
    for slug, p in cat.items():
        best = 0
        for field, w in WEIGHTS:
            v = str(p.get(field, "")).lower()
            if not v or t not in v:
                continue
            # An exact field match outranks the same word buried in a longer
            # string, so "Vult" finds Vult before Vultiverse.
            best = max(best, w + (25 if v == t else 0))
        if best:
            hits[slug] = {
                "name": p.get("name", slug),
                "brand": p.get("brand", ""),
                "premium": bool(p.get("premium")),
                "installed": slug in inv,
                "url": p.get("pluginUrl") or p.get("manualUrl") or "",
                "score": best,
                "why": p.get("description", "") if best <= 20 else "",
            }
    return dict(sorted(hits.items(), key=lambda kv: -kv[1]["score"]))


# ── Generation ───────────────────────────────────────────────────────────────

HERE = os.path.dirname(os.path.abspath(__file__))
CONTRACT = os.path.join(HERE, "prompt", "patch_contract.md")


def render_inventory(inv: dict, prefer: str | None = None) -> str:
    """The inventory as the model sees it, ports included where known."""
    out = []
    order = sorted(inv, key=lambda s: (s != prefer, s))
    for pslug in order:
        p = inv[pslug]
        head = f"### {pslug} — {p['name']}"
        if prefer and pslug == prefer:
            head += "   ← the user's own modules; prefer these when they fit"
        out.append(head)
        for mslug, m in sorted(p["modules"].items()):
            line = f"- `{mslug}` {m['name']}"
            if m.get("description"):
                line += f" — {m['description']}"
            if m.get("tags"):
                line += f"  [{', '.join(m['tags'])}]"
            out.append(line)
            if m.get("inputs"):
                ins = ", ".join(f"{i}={n}" for i, n in enumerate(m["inputs"]))
                outs = ", ".join(f"{i}={n}" for i, n in enumerate(m.get("outputs", [])))
                out.append(f"    in: {ins}")
                out.append(f"    out: {outs}")
        out.append("")
    return "\n".join(out)


def generate(prompt: str, inv: dict, prefer: str | None, retries: int = 2):
    """Prompt -> a patch that lints clean. Returns (patch, why) or raises."""
    import re
    import subprocess
    claude = os.environ.get("FORGE_CLAUDE_BIN", "claude")
    contract = open(CONTRACT).read().replace("<!--INVENTORY-->",
                                             render_inventory(inv, prefer))
    ctx = None
    for attempt in range(retries + 1):
        parts = [contract, "\n---\n\n## Your task\n\nBuild this patch:\n\n> " + prompt]
        if ctx:
            parts.append("\n\n## Your previous attempt was REJECTED. Fix it.\n\n"
                         "Return both blocks again, corrected. Do not explain.\n\n"
                         "```\n" + ctx + "\n```")
        r = subprocess.run([claude, "-p", "\n".join(parts)],
                           capture_output=True, text=True, timeout=600)
        if r.returncode != 0:
            raise SystemExit(f"model call failed: {r.stderr[:400]}")
        pj = re.search(r"```(?:json patch|json)\s*\n(.*?)```", r.stdout, re.S)
        wj = re.search(r"```json why\s*\n(.*?)```", r.stdout, re.S)
        if not pj:
            ctx = "Your reply did not contain a ```json patch block."
            continue
        try:
            patch = json.loads(pj.group(1))
        except json.JSONDecodeError as e:
            ctx = f"The patch block was not valid JSON: {e}"
            continue
        why = {}
        if wj:
            try:
                why = json.loads(wj.group(1))
            except json.JSONDecodeError:
                pass                       # prose is optional; the patch is not
        errs = lint(patch, inv)
        if not errs:
            return patch, why
        print(f"  rejected (attempt {attempt + 1}):", flush=True)
        for e in errs[:5]:
            print(f"    {e}", flush=True)
        ctx = "The patch was rejected:\n" + "\n".join(errs)
    raise SystemExit(f"gave up after {retries + 1} attempts")


# ── CLI ──────────────────────────────────────────────────────────────────────

def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    inv = inventory()

    if cmd == "inventory":
        total = sum(len(p["modules"]) for p in inv.values())
        print(f"{len(inv)} plugin(s), {total} module(s) available to patch with\n")
        for slug, p in sorted(inv.items()):
            named = sum(1 for m in p["modules"].values() if m.get("inputs"))
            extra = f"  ({named} with known port names)" if named else ""
            print(f"  {slug:20} {p['version']:10} {len(p['modules']):3} modules{extra}")
        return 0

    if cmd in ("explain", "lint") and len(argv) > 2:
        patch = json.load(open(argv[2]))
        if cmd == "explain":
            print(explain(patch, inv))
            return 0
        errs = lint(patch, inv)
        for e in errs:
            print(f"  {e}")
        print(f"{'FAIL' if errs else 'ok'}: {len(errs)} problem(s)")
        return 1 if errs else 0

    if cmd == "mention" and len(argv) > 2:
        # Selection-time gating. Anything not installed cannot be wired, so
        # resolving a mention BEFORE generating is what stops a whole patch
        # being built around something the user cannot load.
        cat = catalog()
        hits = resolve_mention(argv[2], cat, inv)
        if not hits:
            print(f"nothing in the library matches '{argv[2]}'")
            return 1
        usable = [s for s, h in hits.items() if h["installed"]]
        print(f"'{argv[2]}' — {len(hits)} match(es), {len(usable)} usable now\n")
        for slug, h in hits.items():
            if h["installed"]:
                state, hint = "✓ ready", ""
            elif h["premium"]:
                # Deliberately not "you need to buy this". A premium plugin the
                # user already owns but has not synced to this machine looks
                # identical from here -- there is no readable signal for
                # ownership -- and telling someone to buy what they own is a
                # worse error than telling them to check.
                state = "$ premium"
                hint = "  → owned? sync it in Rack's Library. Otherwise buy it, or VCV+"
            else:
                state, hint = "↓ free", "  → install from Rack's Library, then rescan"
            print(f"  {state:11} {slug:22} {h['brand'] or h['name']}")
            if hint:
                print(f"{'':13}{hint}")
        if not usable:
            print("\nNone of these are installed, so a patch cannot use them yet.")
            print("Generation would be wasted — install first, then ask again.")
            return 2
        return 0

    if cmd == "catalog":
        cat = catalog(refresh="--refresh" in argv)
        term = next((a for a in argv[2:] if not a.startswith("--")), None)
        if term:
            hits = resolve_mention(term, cat, inv)
            print(f"'{term}' matches {len(hits)} plugin(s):\n")
            for slug, h in hits.items():   # already ranked by match quality
                mark = "installed" if h["installed"] else \
                       ("PREMIUM" if h["premium"] else "free, not installed")
                note = f"  — {h['why'][:60]}" if h.get("why") else ""
                print(f"  {slug:26} {h['brand'] or h['name']:24} {mark}{note}")
            return 0
        prem = sum(1 for p in cat.values() if p.get("premium"))
        have = sum(1 for s in cat if s in inv)
        import time
        age = (time.time() - os.path.getmtime(CATALOG)) / 3600
        print(f"{len(cat)} plugins in the library · {prem} premium · "
              f"{have} installed here")
        print(f"cache {CATALOG} ({age:.1f}h old, refreshed every "
              f"{CATALOG_MAX_AGE_DAYS} days)")
        return 0

    if cmd == "build" and len(argv) > 2:
        # --prefer-ours biases toward the user's own generated modules; without
        # it the whole installed library competes on equal footing, which is
        # usually what you want when a vendor module simply fits better.
        prefer = "ForgeModular" if "--prefer-ours" in argv else None
        out = "/tmp/forge-patch.vcv"
        if "--out" in argv:
            out = argv[argv.index("--out") + 1]
        patch, why = generate(argv[2], inv, prefer)
        json.dump(patch, open(out, "w"), indent=1)
        print(f"  built {len(patch.get('modules', []))} modules, "
              f"{len(patch.get('cables', []))} cables → {out}\n")
        print(explain(patch, inv, why))
        return 0

    if cmd == "diff" and len(argv) > 3:
        d = diff(json.load(open(argv[2])), json.load(open(argv[3])), inv)
        print("\n".join(d) if d else "  no structural change")
        return 0

    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
