#!/usr/bin/env python3
"""A patch as text: readable, editable, round-trippable.

    patch_lang.py show   <file.vcv>            # .vcv  -> text
    patch_lang.py build  <file.pat> <out.vcv>  # text  -> .vcv
    patch_lang.py check  <file.pat>            # parse only, report errors

WHY THIS EXISTS

A .vcv is JSON: module ids, cable endpoints given as integer port INDICES, and
panel coordinates. It is a fine transport format and an impossible authoring
one. Nobody can read it, nobody can edit it, and — the expensive part — asking
a model to emit it means asking for three things it cannot know:

  * panel POSITIONS, which is why generated patches arrived with modules
    overlapping by 2HP until the layout was taken away from the model entirely;
  * port INDICES, where an off-by-one is a silently wrong patch rather than an
    error;
  * cable ids, which carry no meaning and must merely be unique.

None of those are expressible here, so none of them can be wrong.

    # a melodic arpeggiator quantized to the key of g
    lfo   : ForgeModular/LFO
    seq   : ForgeModular/SEQ
    quant : Fundamental/Quantizer
    vco   : ForgeModular/VCO
    out   : Core/AudioInterface2

    lfo.SQR >> seq.CLK
    seq.CV  >> quant.IN >> vco.VOCT   # the quantizer forces every step into G
    vco.SAW >> out.LEFT

HOW OURS DIFFERS FROM THE PRIOR ART

The shape — named nodes, a `>>` chain operator, one text file — is a good idea
and not a new one; Sorbis (AGPL, so read and not borrowed) reaches for it too.
Three things here are deliberately different, and each comes from Pulp knowing
something a fixed-primitive language cannot:

  1. IT IS CHECKED AGAINST REAL MODULES. Every port name is verified against
     the installed inventory, so `vco.VOTC` is a parse error naming the line
     and listing the ports that module actually has — not a patch that loads
     into silence.
  2. THE CHAIN INFERS THE NEXT PORT. `a.OUT >> b.IN >> c.IN` needs b's output,
     and we know it: if a module has exactly one output the chain continues
     through it without being named. Only ambiguity has to be spelled out.
  3. IT ROUND-TRIPS. Text to .vcv and back, so an existing patch can be read,
     edited by hand, and re-rendered without spending a model call. A one-way
     generator gives you a file you cannot revise.

Positions are computed on the way out (see `patch.reflow`), not written here.
"""

from __future__ import annotations

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import patch as P  # noqa: E402


class PatchLangError(Exception):
    """A parse or resolution error, with the line it happened on."""

    def __init__(self, line_no: int, message: str, hint: str = ""):
        self.line_no = line_no
        self.message = message
        self.hint = hint
        super().__init__(f"line {line_no}: {message}" + (f"\n    {hint}" if hint else ""))


# ---------------------------------------------------------------- inventory --

def _ports(inv: dict, plugin: str, model: str) -> tuple:
    """(inputs, outputs) port NAMES for a module, or ([], []) if unknown.

    Names come from the manifest for our own modules and from the CARTOG scan
    for everyone else's. A module nobody has measured has no port names, and
    that is reported rather than guessed: a chain that invents a port produces
    a patch that loads and does nothing.
    """
    entry = (inv.get(plugin, {}).get("modules", {}) or {}).get(model)
    if not entry:
        return [], []

    def names(group):
        # The inventory carries port names as plain strings; a scan-derived one
        # may carry objects. Both shapes appear in the same dict depending on
        # whether the module came from a manifest or from CARTOG, so read both
        # rather than assuming the one in front of you.
        out = []
        for item in (entry.get(group) or []):
            if isinstance(item, str):
                out.append(item)
            elif isinstance(item, dict):
                out.append(item.get("name", ""))
        return [n for n in out if n]

    return names("inputs"), names("outputs")


def _params(inv: dict, plugin: str, model: str) -> list:
    """Param NAMES for a module, ordered by id. Empty when unmeasured.

    Our own modules carry name/min/max/default in the manifest. A vendor
    module's params come from the CARTOG scan and may be nameless, in which
    case the language falls back to the index — a value written against an
    index still round-trips, it is merely less readable, and dropping the
    value instead would lose the patch's sound.
    """
    entry = (inv.get(plugin, {}).get("modules", {}) or {}).get(model)
    if not entry:
        return []
    out = []
    for e in (entry.get("params") or []):
        if isinstance(e, dict):
            out.append((e.get("id", len(out)), e.get("name") or ""))
        elif isinstance(e, str):
            out.append((len(out), e))
    out.sort(key=lambda t: t[0])
    return [n for _, n in out]


def _port_index(names: list, want: str) -> int:
    """Index of a port by name, case-insensitively. -1 when absent.

    `NAME#2` selects the SECOND port with that name. Some modules genuinely
    have two: a DUAL AD has two TRIG inputs and two ENV outputs, one per
    channel, and FOLD has two CV inputs. A name-keyed language cannot tell
    them apart, and quietly resolving both to the first is not a naming
    problem — it is a patch wired to the wrong channel.
    """
    want = want.strip()
    occurrence = 1
    if "#" in want:
        want, _, suffix = want.rpartition("#")
        if suffix.isdigit():
            occurrence = int(suffix)
        else:
            want = want + "#" + suffix
    low = want.strip().lower()
    seen = 0
    for i, n in enumerate(names):
        if n.strip().lower() == low:
            seen += 1
            if seen == occurrence:
                return i
    return -1


def port_label(names: list, index: int) -> str:
    """How to WRITE the port at `index` so it reads back to the same port."""
    if not (0 <= index < len(names)):
        return str(index)
    name = names[index]
    same = [i for i, n in enumerate(names) if n == name]
    if len(same) == 1:
        return name
    return f"{name}#{same.index(index) + 1}"


# ------------------------------------------------------------------- parsing --

DECL = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([A-Za-z0-9_\-]+)/([A-Za-z0-9_\-]+)\s*$")
# A port name is whatever its author called it, and vendors call them things
# like `A IN`, `Channel 1` and `To "device output 1"`. A grammar that only
# accepts bare identifiers renders those and then cannot read them back, so the
# language would print files it could not parse. Quoted form covers everything.
ENDPOINT = re.compile(
    r"""^([A-Za-z_][A-Za-z0-9_]*)                 # the local name
        (?:\.(?:"((?:[^"\\]|\\.)*)"|([A-Za-z0-9_/\-]+)))?$""",  # .PORT or ."a port"
    re.VERBOSE)


def quote_port(name: str) -> str:
    """A port as it must be written to survive a round trip.

    Rack's audio interface names a port `To "device output 1"` — quotes and
    all — so wrapping in quotes without escaping produced
    `."To "device output 1""`, which reads back as three tokens and a syntax
    error. The file the renderer prints must be a file the parser accepts;
    anything else is a round trip that only looks like one.
    """
    if re.fullmatch(r"[A-Za-z0-9_/\-]+", name):
        return name
    return '"' + name.replace("\\", "\\\\").replace('"', '\\"') + '"'


def unquote_port(raw: str) -> str:
    return raw.replace('\\"', '"').replace("\\\\", "\\")


def parse(text: str, inv: dict) -> dict:
    """Text -> a patch dict, or raise PatchLangError.

    Errors name the line and, where it helps, what WOULD have been valid. A
    parser that says only "invalid" for a typo in a port name has moved the
    work back onto the person.
    """
    names: dict = {}          # local name -> (plugin, model)
    order: list = []          # declaration order, which is left-to-right order
    cables: list = []
    settings: list = []
    request = ""

    for n, raw in enumerate(text.splitlines(), start=1):
        # Strip comments OUTSIDE quotes: a port called `To "device output 1"`
        # contains no #, but one day something will, and a comment stripper
        # that does not know about quotes is a silent corruption waiting.
        line, in_q, esc = [], False, False
        for ch in raw:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_q = not in_q
            elif ch == "#" and not in_q and not (line and
                    (line[-1].isalnum() or line[-1] in "_-/\"")):
                # A `#` right after a port name is an occurrence suffix
                # (TRIG#2), not a comment. Only a `#` that starts a word is.
                break
            line.append(ch)
        line = "".join(line).rstrip()
        if not line.strip():
            # A leading comment block is the REQUEST. Kept, because a patch
            # that cannot say what it was asked to do makes the reader guess.
            if not request and raw.strip().startswith("#"):
                request = raw.strip().lstrip("#").strip()
            continue
        if raw.strip().startswith("#") and not request:
            request = raw.strip().lstrip("#").strip()

        decl = DECL.match(line)
        if decl:
            local, plugin, model = decl.group(1), decl.group(2), decl.group(3)
            if local in names:
                raise PatchLangError(n, f"'{local}' is declared twice")
            if plugin not in inv:
                raise PatchLangError(
                    n, f"no plugin '{plugin}' is installed",
                    "installed: " + ", ".join(sorted(inv)[:6]) + "…")
            if model not in (inv[plugin].get("modules") or {}):
                have = sorted((inv[plugin].get("modules") or {}))[:6]
                raise PatchLangError(
                    n, f"'{plugin}' has no module '{model}'",
                    "it has: " + ", ".join(have) + "…")
            names[local] = (plugin, model)
            order.append(local)
            continue

        if ">>" in line:
            cables.extend(_parse_chain(n, line, names, inv))
            continue

        if "=" in line:
            settings.append(_parse_setting(n, line, names, inv))
            continue

        raise PatchLangError(
            n, f"cannot read {line.strip()!r}",
            "a line is either  name : Plugin/Module  or  a.OUT >> b.IN")

    if not order:
        raise PatchLangError(1, "no modules declared")

    return _assemble(names, order, cables, settings, inv, request)


def _parse_chain(n: int, line: str, names: dict, inv: dict) -> list:
    """One `a.OUT >> b.IN >> c.IN` line into cables.

    The middle of a chain needs the module's OUTPUT, which the writer has not
    named. With one output it is unambiguous and inferring it is the whole
    ergonomic win; with several, refusing and listing them is the only honest
    answer.
    """
    hops = [h.strip() for h in line.split(">>")]
    out = []
    for i in range(len(hops) - 1):
        src_txt, dst_txt = hops[i], hops[i + 1]
        # A chain may fan out at its end: `>> out.LEFT, out.RIGHT`.
        for dst_one in [d.strip() for d in dst_txt.split(",") if d.strip()]:
            src_name, src_port = _endpoint(n, src_txt, names)
            dst_name, dst_port = _endpoint(n, dst_one, names)
            s_in, s_out = _ports(inv, *names[src_name])
            d_in, d_out = _ports(inv, *names[dst_name])

            if src_port is None:
                if len(s_out) == 1:
                    src_port = s_out[0]
                elif not s_out:
                    raise PatchLangError(
                        n, f"'{src_name}' has no measured outputs",
                        "put it on screen in Rack and press SCAN, or name the "
                        "port explicitly")
                else:
                    raise PatchLangError(
                        n, f"'{src_name}' has {len(s_out)} outputs — say which",
                        "one of: " + ", ".join(s_out))
            if dst_port is None:
                if len(d_in) == 1:
                    dst_port = d_in[0]
                else:
                    raise PatchLangError(
                        n, f"'{dst_name}' has {len(d_in)} inputs — say which",
                        "one of: " + ", ".join(d_in) if d_in else
                        "it has no measured inputs")

            if _port_index(s_out, src_port) < 0:
                raise PatchLangError(
                    n, f"'{src_name}' has no output '{src_port}'",
                    "its outputs: " + (", ".join(s_out) or "none measured"))
            if _port_index(d_in, dst_port) < 0:
                raise PatchLangError(
                    n, f"'{dst_name}' has no input '{dst_port}'",
                    "its inputs: " + (", ".join(d_in) or "none measured"))
            out.append((src_name, src_port, dst_name, dst_port))
        # The chain continues from that module's OUTPUT, which is inferred —
        # carrying the endpoint forward as written (`b.IN`) made the next hop
        # look for an OUTPUT called IN and refuse a chain that is the whole
        # point of the syntax.
        nxt = dst_txt.split(",")[0].strip()
        m = ENDPOINT.match(nxt)
        hops[i + 1] = m.group(1) if m else nxt
    return out




SETTING = re.compile(
    r"""^([A-Za-z_][A-Za-z0-9_]*)\.(?:"((?:[^"\\]|\\.)*)"|([A-Za-z0-9_/\-\#]+))
        \s*=\s*(-?[0-9.eE+]+)\s*$""", re.VERBOSE)


def _parse_setting(n: int, line: str, names: dict, inv: dict) -> tuple:
    """`name.PARAM = 0.34` — a knob position.

    A patch without its values is a wiring diagram. The acid line in the
    regression corpus is acid because its resonance is 0.88 and its decay is
    0.07; a language that carried only the cables would have described the same
    modules wired the same way, making a completely different sound.
    """
    m = SETTING.match(line)
    if not m:
        raise PatchLangError(
            n, f"cannot read the setting {line.strip()!r}",
            "a setting looks like  name.PARAM = 0.5")
    local = m.group(1)
    pname = unquote_port(m.group(2)) if m.group(2) is not None else m.group(3)
    if local not in names:
        raise PatchLangError(n, f"'{local}' was never declared")
    params = _params(inv, *names[local])
    idx = _port_index(params, pname)
    if idx < 0 and pname.isdigit():
        idx = int(pname)          # unnamed vendor params address by index
    if idx < 0:
        raise PatchLangError(
            n, f"'{local}' has no parameter '{pname}'",
            "its parameters: " + (", ".join(p for p in params if p) or
                                  "none are named; use an index"))
    try:
        value = float(m.group(4))
    except ValueError:
        raise PatchLangError(n, f"{m.group(4)!r} is not a number") from None
    return (local, idx, value)


def _endpoint(n: int, txt: str, names: dict) -> tuple:
    m = ENDPOINT.match(txt)
    if not m:
        raise PatchLangError(n, f"cannot read the endpoint {txt!r}",
                             "endpoints look like  name  or  name.PORT")
    local = m.group(1)
    port = unquote_port(m.group(2)) if m.group(2) is not None else m.group(3)
    if local not in names:
        raise PatchLangError(
            n, f"'{local}' was never declared",
            "declared: " + ", ".join(sorted(names)) if names else
            "nothing is declared yet")
    return local, port


def _assemble(names: dict, order: list, cables: list, settings: list,
              inv: dict, request: str) -> dict:
    """Named nodes and chains into the .vcv shape."""
    ids = {local: i + 1 for i, local in enumerate(order)}
    modules = []
    for local in order:
        plugin, model = names[local]
        vals = {idx: v for (who, idx, v) in settings if who == local}
        modules.append({"id": ids[local], "plugin": plugin, "model": model,
                        "pos": [0, 0],
                        "params": [{"id": i, "value": vals[i]}
                                   for i in sorted(vals)]})
    out_cables = []
    for cid, (sn, sp, dn, dp) in enumerate(cables, start=1):
        s_in, s_out = _ports(inv, *names[sn])
        d_in, d_out = _ports(inv, *names[dn])
        out_cables.append({
            "id": cid,
            "outputModuleId": ids[sn], "outputId": _port_index(s_out, sp),
            "inputModuleId": ids[dn], "inputId": _port_index(d_in, dp),
        })
    doc = {"version": "2.6.6", "modules": modules, "cables": out_cables}
    # Positions are OURS, never the author's: this is the whole reason the
    # language has no way to express them.
    P.reflow(doc, inv)
    if request:
        doc["forge_request"] = request
    return doc


# ------------------------------------------------------------------ printing --

def render(doc: dict, inv: dict) -> str:
    """A patch dict -> text. The inverse of parse(), so a patch round-trips."""
    lines = []
    request = doc.get("forge_request")
    if request:
        lines += [f"# {request}", ""]

    # Names a person would choose: the model slug, lowercased, numbered only
    # when it repeats. `vco`, `vco2` reads; `module_7` does not.
    local_of, used = {}, {}
    for m in doc.get("modules", []):
        base = re.sub(r"[^a-z0-9]", "", str(m.get("model", "m")).lower()) or "m"
        # A local name is an identifier, and plenty of module slugs are not:
        # `8vert`, `4Rounds`. Rendering one produced a file this parser then
        # refused to read — a round trip that only looked like one.
        if base[0].isdigit():
            base = "m" + base
        used[base] = used.get(base, 0) + 1
        local_of[m["id"]] = base if used[base] == 1 else f"{base}{used[base]}"

    width = max((len(v) for v in local_of.values()), default=1)
    for m in doc.get("modules", []):
        lines.append(f"{local_of[m['id']]:<{width}} : "
                     f"{m.get('plugin')}/{m.get('model')}")
    # Values, under the declarations they belong to.
    wrote_any = False
    for m in doc.get("modules", []):
        names = _params(inv, m.get("plugin"), m.get("model"))
        for prm in (m.get("params") or []):
            if not isinstance(prm, dict) or "value" not in prm:
                continue
            i = int(prm.get("id", -1))
            label = port_label(names, i) if 0 <= i < len(names) and names[i] \
                else str(i)
            lines.append(f"{local_of[m['id']]}.{quote_port(label)} = "
                         f"{prm['value']:g}")
            wrote_any = True
    if wrote_any:
        lines.append("")
    lines.append("") if not wrote_any else None

    by_id = {m["id"]: m for m in doc.get("modules", [])}
    for c in doc.get("cables", []):
        src, dst = by_id.get(c.get("outputModuleId")), by_id.get(c.get("inputModuleId"))
        if not src or not dst:
            continue
        _, s_out = _ports(inv, src.get("plugin"), src.get("model"))
        d_in, _ = _ports(inv, dst.get("plugin"), dst.get("model"))
        sp = port_label(s_out, c.get("outputId", -1))
        dp = port_label(d_in, c.get("inputId", -1))
        lines.append(f"{local_of[src['id']]}.{quote_port(sp)} >> "
                     f"{local_of[dst['id']]}.{quote_port(dp)}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------- cli --

def main(argv: list) -> int:
    if len(argv) < 3:
        print(__doc__.strip().splitlines()[0])
        print("\n".join(__doc__.splitlines()[2:5]))
        return 2
    cmd, path = argv[1], argv[2]
    inv = P.inventory()

    if cmd == "show":
        print(render(json.load(open(path)), inv), end="")
        return 0

    if cmd in ("check", "build"):
        try:
            doc = parse(open(path).read(), inv)
        except PatchLangError as e:
            print(f"  {e}", file=sys.stderr)
            return 1
        errs = P.lint(doc, inv)
        for e in errs:
            print(f"  {e}", file=sys.stderr)
        if errs:
            return 1
        if cmd == "check":
            print(f"  ok: {len(doc['modules'])} modules, "
                  f"{len(doc['cables'])} cables")
            return 0
        json.dump(doc, open(argv[3], "w"), indent=1)
        print(f"  built {len(doc['modules'])} modules, "
              f"{len(doc['cables'])} cables → {argv[3]}")
        return 0

    print(f"unknown command {cmd!r}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
