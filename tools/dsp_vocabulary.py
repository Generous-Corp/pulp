#!/usr/bin/env python3
"""Render the legacy `pulp::signal` vocabulary from the capability manifest.

This lives in Pulp's tools/ rather than beside any one consumer, because more
than one thing needs it and none of them should own it:

  * the Rack module generator feeds it to a model, so the model is told what
    Pulp really provides instead of guessing;
  * Forge's graph -> C++ exporter validates its emitter table against it.

The installed agent-capability manifest is now the consumer-facing authority.
`scan_headers()` remains private regeneration plumbing used by the manifest
writer/freshness gate; normal JSON and Markdown output read the checked
manifest projection so consumers cannot observe a competing inventory.

Output: markdown for prompting, or `--json` for machine consumers.

    dsp_vocabulary.py            # markdown, for a prompt
    dsp_vocabulary.py --json     # {header: [{class, methods}]}, for a validator
"""
from __future__ import annotations

import json
import os
import pathlib
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

SIGNAL = os.path.normpath(os.path.join(HERE, "..", "core", "signal",
                                       "include", "pulp", "signal"))

# Grouped so the prompt reads as "here is what you reach for", not a flat dump.
GROUPS = [
    ("Oscillators", ["oscillator", "osc/", "wavetable", "lfo", "phase_distortion",
                     "additive_bank", "square_osc_bank"]),
    ("Filters", ["svf", "ladder_filter", "analog_vcf", "tpt_filter",
                 "ota_cascade_filter", "linkwitz_riley", "biquad"]),
    ("Envelopes & dynamics", ["adsr", "envelope", "decay_envelope",
                              "ballistics_filter", "compressor", "noise_gate"]),
    ("Amplitude & routing", ["vca", "gain", "crossfade", "panner",
                             "lowpass_gate", "vactrol", "smoothed_value"]),
    ("CV utilities", ["mod_tools", "scale_quantizer", "harmony_engine"]),
    ("Clocks, gates & triggers", ["trigger", "gate_logic", "probability_gate"]),
    ("Sequencing", ["modular_sequencing", "stage_sequencer", "cartesian_walk",
                    "rungler"]),
    ("Noise & output", ["noise_source", "chaos", "rng", "dither",
                        "velvet_noise", "lofi_chain"]),
    ("Drums", ["drum/"]),
    ("Physical modeling", ["karplus_strong", "modal_bank", "bridged_t_resonator"]),
    ("Effects", ["delay_line", "character_delay", "chorus", "flanger", "phaser",
                 "reverb", "fdn_reverb", "waveshaper", "distortion", "saturator",
                 "fuzz_pair", "granular", "pitch_shifter", "frequency_shifter_ssb",
                 "vocoder", "dc_blocker"]),
    ("Analysis", ["yin_tracker", "envelope_follower"]),
]

# Lifecycle calls are not noise to a code generator. Omitting `reset` was
# especially destructive: a resettable phase type was advertised without the
# one method the request needed, so the model invented a replacement class.
BORING: set[str] = set()

QUERY_STOPWORDS = {
    "and", "for", "from", "generator", "module", "that", "the", "with",
}

MODULE_CAPABILITIES = pathlib.Path(HERE) / "rack" / "knowledge" / "module" / "dsp-primitives.json"

QUERY_ALIASES = {
    "clock": {"clock", "divider", "gate", "phase", "pulse", "trigger"},
    "rate": {"frequency", "phase", "oscillator", "clock"},
    "phase": {"phase", "oscillator"},
    "width": {"pulse", "square", "width"},
    "reset": {"reset", "trigger", "gate", "phase"},
    "pitch": {"pitch", "frequency", "quantizer", "oscillator"},
    "filter": {"filter", "svf", "vcf", "ladder"},
    "envelope": {"envelope", "adsr", "decay", "slew"},
    "delay": {"delay", "echo", "feedback"},
    "reverb": {"reverb", "fdn"},
    "noise": {"noise", "random", "rng"},
    "random": {"random", "rng", "probability", "noise"},
    "sequencer": {"sequence", "sequencer", "stage", "clock"},
    "drum": {"drum", "trigger", "envelope"},
}


def _words(value: str) -> set[str]:
    value = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", value)
    return set(re.findall(r"[a-z][a-z0-9]+", value.lower()))


def module_capability_problems(found: dict) -> list[str]:
    """Validate the whole curated registry, including currently unused rows."""
    try:
        document = json.loads(MODULE_CAPABILITIES.read_text())
    except (OSError, json.JSONDecodeError) as error:
        return [f"cannot read module capability registry: {error}"]
    if document.get("schema") != "forge.module-dsp-capabilities.v1":
        return ["module capability registry has the wrong schema"]
    problems, seen_ids, seen_types = [], set(), set()
    for index, capability in enumerate(document.get("capabilities", [])):
        where = f"module capability {index}"
        identifier = capability.get("id")
        if not isinstance(identifier, str) or not identifier:
            problems.append(f"{where} has no id")
        elif identifier in seen_ids:
            problems.append(f"duplicate module capability id {identifier}")
        seen_ids.add(identifier)
        relative, cls = capability.get("include"), capability.get("class")
        key = (relative, cls)
        if key in seen_types:
            problems.append(f"duplicate module capability type {relative}:{cls}")
        seen_types.add(key)
        matches = [row for row in found.get(relative, [])
                   if row.get("class") == cls]
        if len(matches) != 1:
            problems.append(f"{identifier} does not resolve exactly to {relative}:{cls}")
        elif "::detail::" in matches[0].get("qualified_name", ""):
            problems.append(f"{identifier} exposes a private detail type")
        if not capability.get("intent"):
            problems.append(f"{identifier} has no intent vocabulary")
        if capability.get("role") not in {"primary", "helper"}:
            problems.append(f"{identifier} must declare primary/helper role")
        if not str(capability.get("behavior", "")).strip():
            problems.append(f"{identifier} has no behavior contract")
        header = pathlib.Path(SIGNAL) / str(relative)
        try:
            header_text = header.read_text()
        except OSError as error:
            problems.append(f"{identifier} header cannot be read: {error}")
            header_text = ""
        for enum in capability.get("associated_enums", []):
            enum_name = str(enum.get("name", "")).split("::")[-1]
            match = re.search(rf"enum\s+class\s+{re.escape(enum_name)}\b[^{{]*\{{([^}}]*)\}}",
                              header_text)
            actual = [] if not match else [
                part.split("=", 1)[0].strip()
                for part in re.sub(r"//[^\n]*", "", match.group(1)).split(",")
                if re.fullmatch(r"[A-Za-z_]\w*",
                                part.split("=", 1)[0].strip())]
            if actual != enum.get("values"):
                problems.append(f"{identifier} associated enum {enum_name} is stale")
    return problems


def shortlist(found: dict, prompt: str, limit: int = 10) -> dict:
    """Return a compact, exact legal API set relevant to one request.

    The full compatibility inventory is useful for discovery and freshness,
    but sending hundreds of unrelated types to a generator spends context and
    makes plausible near-name guesses more likely. Ranking never invents an
    API: every selected row is copied verbatim from the checked manifest.
    """
    query = _words(prompt) - QUERY_STOPWORDS
    expanded = set(query)
    for word in query:
        expanded.update(QUERY_ALIASES.get(word, ()))
    problems = module_capability_problems(found)
    if problems:
        raise RuntimeError("; ".join(problems))
    document = json.loads(MODULE_CAPABILITIES.read_text())
    ranked = []
    for capability in document.get("capabilities", []):
        intent = set(capability.get("intent", []))
        direct = len(query & intent)
        baseline = bool(capability.get("baseline"))
        if not direct and not baseline:
            continue
        score = direct * 20 + len(expanded & intent) * 4
        if baseline:
            score += 1
        if score:
            ranked.append((-score, capability["id"], capability))
    ranked.sort()
    chosen = [capability for _, _, capability in ranked[:limit]]
    out: dict[str, list[dict]] = {}
    for capability in chosen:
        relative = capability["include"]
        matches = [row for row in found.get(relative, [])
                   if row.get("class") == capability["class"]]
        if len(matches) != 1:
            raise RuntimeError(
                f"module DSP capability {capability['id']} no longer resolves "
                f"exactly to {relative}:{capability['class']}")
        row = dict(matches[0])
        row["capability"] = capability["id"]
        row["capability_role"] = capability["role"]
        row["behavior_contract"] = capability["behavior"]
        row["direct_match"] = bool(query & set(capability.get("intent", [])))
        row["associated_enums"] = capability.get("associated_enums", [])
        out.setdefault(relative, []).append(row)
    return out


def public_enums(text: str, cls: str):
    """Enum types declared inside `cls`, as JSON-shaped [name, [values]].

    Without these the model sees `set_mode(Mode m)` and has to guess what a
    Mode is. It guesses plausibly and wrongly -- `Mode::Independent`,
    `Mode::Exponential` -- and the run dies at the compiler after the model has
    already been paid for. A method signature that names a type is only useful
    alongside the type's values.
    """
    m = re.search(rf"(?:class|struct)\s+{re.escape(cls)}\b", text)
    if not m:
        return []
    body, depth, i, started = [], 0, m.end(), False
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
            started = True
        elif c == "}":
            depth -= 1
            if started and depth == 0:
                break
        if started:
            body.append(c)
        i += 1
    src = "".join(body)
    out = []

    def values_from(block: str):
        # Remove comments before comma tokenization. If a line comment follows
        # one enumerator, splitting first makes that comment share a fragment
        # with the next line's enumerator and silently drops the latter.
        block = re.sub(r"/\*.*?\*/", "", block, flags=re.S)
        block = re.sub(r"//[^\n]*", "", block)
        values = []
        for raw in block.split(","):
            raw = raw.split("=", 1)[0].strip()
            if re.fullmatch(r"[A-Za-z_]\w*", raw):
                values.append(raw)
        return values

    # A class often aliases a file-level enum -- `using Mode = SlewMode;` --
    # so the values live outside the body the caller sees. Follow the alias,
    # and report it under the name the METHOD signature uses, since that is
    # the name the model has to write.
    aliased = []
    for am in re.finditer(r"using\s+(\w+)\s*=\s*(\w+)\s*;", src):
        local, target = am.group(1), am.group(2)
        em = re.search(rf"enum\s+class\s+{re.escape(target)}\b[^{{]*\{{([^}}]*)\}}", text)
        if em:
            aliased.append((local, em.group(1)))

    for local, block in aliased:
        values = values_from(block)
        if values:
            out.append([local, values])

    for em in re.finditer(r"enum\s+class\s+(\w+)[^{]*\{([^}]*)\}", src):
        name = em.group(1)
        values = values_from(em.group(2))
        if values:
            out.append([name, values])
    return out


def code_only(text: str) -> str:
    """Blank comments and literals while preserving offsets and newlines."""
    def is_digit_separator(position: int) -> bool:
        following = text[position + 1] if position + 1 < len(text) else ""
        previous = text[position - 1] if position > 0 else ""
        if previous.isdigit() and following.isdigit():
            return True
        start = position - 1
        while start >= 0 and (text[start].isalnum() or text[start] in "_.'"):
            start -= 1
        prefix = text[start + 1:position].replace("'", "")
        if re.fullmatch(
            r"0[xX](?:[0-9A-Fa-f]+(?:\.[0-9A-Fa-f]*)?|\.[0-9A-Fa-f]+)", prefix
        ):
            return following in "0123456789abcdefABCDEF"
        if re.fullmatch(r"0[bB][01]+", prefix):
            return following in "01"
        return False

    out = list(text)
    index = 0
    state = None
    while index < len(text):
        char = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if state is None:
            if char == "/" and following == "/":
                state = "line"
                out[index] = out[index + 1] = " "
                index += 2
                continue
            if char == "/" and following == "*":
                state = "block"
                out[index] = out[index + 1] = " "
                index += 2
                continue
            if char == '"':
                state = "string"
                out[index] = " "
                index += 1
                continue
            if char == "'":
                if is_digit_separator(index):
                    index += 1
                    continue
                state = "character"
                out[index] = " "
                index += 1
                continue
            index += 1
            continue
        if state == "line":
            if char == "\n":
                state = None
            else:
                out[index] = " "
            index += 1
            continue
        if state == "block":
            if char == "*" and following == "/":
                out[index] = out[index + 1] = " "
                index += 2
                state = None
                continue
            if char != "\n":
                out[index] = " "
            index += 1
            continue
        if char == "\\":
            out[index] = " "
            if index + 1 < len(text) and text[index + 1] != "\n":
                out[index + 1] = " "
                index += 2
            else:
                index += 1
            continue
        if (state == "string" and char == '"') or (state == "character" and char == "'"):
            out[index] = " "
            state = None
            index += 1
            continue
        if char != "\n":
            out[index] = " "
        index += 1
    return "".join(out)


def matching_brace(text: str, opening: int) -> int:
    depth = 0
    for pos in range(opening, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return pos
    raise ValueError("unclosed class body")


def opens_lambda_body(text: str, opening: int) -> bool:
    """Whether a parameter-list brace starts a lambda rather than an initializer."""
    prefix = text[max(0, opening - 512):opening]
    suffix = re.compile(
        r"(?:\s*<[^{};]*>)?"
        r"\s*(?:\([^{};]*\))?"
        r"\s*(?:(?:mutable|constexpr|consteval)\s+)*"
        r"(?:noexcept(?:\s*\([^{};]*\))?\s*)?"
        r"(?:\s*\[\[[^{};]*\]\])*"
        r"(?:\s*->\s*[^{};]+?)?"
        r"(?:\s*requires\b[\s\S]+)?"
        r"(?:\s*\[\[[^{};]*\]\])*\s*$",
    )
    for closing in range(len(prefix) - 1, -1, -1):
        if prefix[closing] != "]" or suffix.fullmatch(prefix[closing + 1:]) is None:
            continue
        depth = 0
        for pos in range(closing, -1, -1):
            if prefix[pos] == "]":
                depth += 1
            elif prefix[pos] == "[":
                depth -= 1
                if depth == 0:
                    return True
    return False


def opens_requires_expression_body(text: str, opening: int) -> bool:
    """Whether a parameter-list brace starts a requires-expression body."""
    prefix = text[max(0, opening - 512):opening]
    return re.search(r"\brequires\s*(?:\([^{};]*\))?\s*$", prefix) is not None


def public_declarations(text: str, cls: str, *, source_is_code: bool = False) -> str:
    """Return only top-level public class text, with method bodies blanked."""
    source = text if source_is_code else code_only(text)
    declaration = re.search(rf"\b(class|struct)\s+{re.escape(cls)}\b[^{{]*{{", source)
    if declaration is None:
        return ""
    opening = source.find("{", declaration.start())
    body = source[opening + 1:matching_brace(source, opening)]
    public = declaration.group(1) == "struct"
    depth = 0
    parameter_depth = 0
    parameter_brace_depth = 0
    parameter_lambda_depth = 0
    out = list(" " * len(body))
    i = 0
    while i < len(body):
        if depth == 0:
            access = re.match(r"(public|private|protected)\s*:", body[i:])
            if access:
                public = access.group(1) == "public"
                i += access.end()
                continue
        char = body[i]
        if parameter_lambda_depth > 0:
            if char == "{":
                parameter_lambda_depth += 1
            elif char == "}":
                parameter_lambda_depth -= 1
                if public and parameter_lambda_depth == 0:
                    out[i] = char
            i += 1
            continue
        depth_before = depth
        method_body_open = char == "{" and parameter_depth == 0
        if depth == 0 and char == "(":
            parameter_depth += 1
        elif depth == 0 and char == ")":
            parameter_depth = max(0, parameter_depth - 1)
        elif depth == 0 and char == "{" and parameter_depth > 0:
            if opens_requires_expression_body(body, i) or opens_lambda_body(body, i):
                parameter_lambda_depth = 1
            else:
                parameter_brace_depth += 1
        elif depth == 0 and char == "}" and parameter_brace_depth > 0:
            parameter_brace_depth -= 1
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
        if public and depth_before == 0 and not method_body_open:
            out[i] = char
        i += 1
    return "".join(out)


def public_methods(text: str, cls: str, *, source_is_code: bool = False):
    """Public methods of `cls`, in declaration order, as `name(args)`."""
    src = public_declarations(text, cls, source_is_code=source_is_code)
    out = []
    for mm in re.finditer(
            r"^\s*(?:\[\[[^\]]*\]\]\s*)?(?:inline\s+|static\s+|constexpr\s+|virtual\s+)*"
            r"([A-Za-z_][\w:<>,\s\*&]*?)\s+([a-z_]\w*)\s*"
            r"\(((?:[^();]|\([^()]*\))*)\)",
            src, re.M):
        ret, name, args = mm.group(1).strip(), mm.group(2), mm.group(3).strip()
        if name.startswith("operator"):
            continue
        args = re.sub(r"\s+", " ", args)
        out.append(f"{name}({args})" if args else f"{name}()")
    # Control-flow keywords look like calls to a regex. Listing `if(rising)` as
    # a method invites the model to call it.
    kKeywords = ("if(", "for(", "while(", "switch(", "return(", "sizeof(")
    out = [s for s in out if not s.startswith(kKeywords)]
    seen, uniq = set(), []
    for s in out:
        if s not in seen:
            seen.add(s)
            uniq.append(s)
    # Core lifecycle always comes first. BeatRepeatKernelT additionally needs
    # its direct gesture controls promoted because they replace the old event
    # API; retain declaration order for every unrelated type so this targeted
    # contract repair does not crowd established accessors out of the cap.
    lifecycle = ("prepare(", "reset(", "process(", "set_immediate(")
    first = [method for method in uniq if method.startswith(lifecycle)]
    if cls == "BeatRepeatKernelT":
        gestures = ("trigger(", "stop(", "seek(", "snapshot(", "restore(")
        direct = [method for method in uniq if method.startswith(gestures)]
        setters = [method for method in uniq
                   if method.startswith("set_") and method not in first]
        rest = [method for method in uniq
                if method not in first and method not in direct and method not in setters]
        prioritized = first + direct + setters + rest
    else:
        prioritized = first + [method for method in uniq if method not in first]
    # 14 rather than 7: the cap exists to keep the prompt readable, and half of
    # these classes have more than seven methods worth knowing about.
    return prioritized[:14]


def _namespace_at(text: str, position: int) -> str:
    """Return the C++ namespace enclosing ``position``.

    Track every brace, not just namespace braces, so a namespace mentioned in
    an inline method cannot leak into a later class identity. Comments and
    literals are masked first because their example braces are not syntax.
    """
    masked = code_only(text[:position])
    stack: list[str | None] = []
    token = re.compile(
        r"(?:inline\s+)?namespace\s+([A-Za-z_]\w*(?:::\w+)*)\s*\{|[{}]"
    )
    for match in token.finditer(masked):
        if match.group(1):
            stack.append(match.group(1))
        elif match.group() == "{":
            stack.append(None)
        elif stack:
            stack.pop()
    return "::".join(item for item in stack if item)


def scan_text(text: str) -> list[dict]:
    """Extract exact callable identities from one public signal header."""
    classes = []
    pattern = re.compile(
        r"^[ \t]*(?:(template\s*<([^;{}]*)>\s*))?"
        r"(?:class|struct)\s+(\w+T?)\b(?!\s*;)",
        re.M,
    )
    for match in pattern.finditer(text):
        cls = match.group(3)
        if cls.endswith("Params") or cls.startswith("_"):
            continue
        methods = public_methods(text, cls)
        if not methods:
            continue
        namespace = _namespace_at(text, match.start())
        classes.append({
            "class": cls,
            "qualified_name": f"{namespace}::{cls}" if namespace else cls,
            "template": (re.sub(r"\s+", " ", match.group(2)).strip()
                         if match.group(1) else None),
            "methods": methods,
            "enums": public_enums(text, cls),
        })
    return classes


def scan_headers():
    """Regeneration input; consumers use scan(), which reads the manifest."""
    scripts = os.path.join(HERE, "scripts")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    from agent_capability_registry import (
        LEGACY_SIGNAL_VOCABULARY_EXCLUSIONS,
        REVIEWED_HEADERS,
    )

    prefix = "pulp/signal/"
    reviewed = {entry["include"]: entry for entry in REVIEWED_HEADERS}
    exclusions = set()
    for include in LEGACY_SIGNAL_VOCABULARY_EXCLUSIONS:
        if not isinstance(include, str) or not include.startswith(prefix) or not include.endswith(".hpp"):
            raise RuntimeError(f"invalid legacy signal vocabulary exclusion: {include!r}")
        review = reviewed.get(include)
        if (review is None or review.get("disposition") != "unsupported_capability" or
                review.get("capability_keys") != []):
            raise RuntimeError(
                "legacy signal vocabulary exclusion must be a reviewed unsupported_capability "
                f"with no capability keys: {include}"
            )
        exclusions.add(include[len(prefix):])
    found = {}
    for root, _, files in os.walk(SIGNAL):
        for fn in sorted(files):
            if not fn.endswith(".hpp"):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, SIGNAL)
            if rel in exclusions:
                continue
            with open(path, encoding="utf-8", errors="ignore") as source:
                text = source.read()
            classes = scan_text(text)
            if classes:
                found[rel] = classes
    # os.walk() preserves the filesystem's directory enumeration order, which
    # differs between filesystems (for example APFS and ext4).  This mapping is
    # serialized into the checked agent-capability manifest, so make its order
    # source-derived rather than environment-derived.
    missing = exclusions - set(
        os.path.relpath(os.path.join(root, fn), SIGNAL)
        for root, _, files in os.walk(SIGNAL)
        for fn in files if fn.endswith(".hpp")
    )
    if missing:
        raise RuntimeError(f"legacy signal vocabulary exclusions are missing: {sorted(missing)}")
    return {relative: found[relative] for relative in sorted(found)}


def scan():
    manifest = pathlib.Path(HERE).parent / "docs/status/agent-capabilities.json"
    document = json.loads(manifest.read_text())
    return document["compatibility"]["signal_vocabulary"]["entries"]


def grouped(found):
    out, used = [], set()
    for title, pats in GROUPS:
        rows = []
        for rel in sorted(found):
            if rel in used:
                continue
            if any(p in rel for p in pats):
                used.add(rel)
                rows.append((rel, found[rel]))
        if rows:
            out.append((title, rows))
    rest = [(r, found[r]) for r in sorted(found) if r not in used]
    if rest:
        out.append(("Other", rest))
    return out


def markdown(found):
    L = ["The identities below are exact. A `template` line means the type is a template;",
         "a type with no such line must be used without `<float>`. All are **per-sample**",
         "and allocation-free. **Prefer these over hand-writing DSP**: they are tested, they are",
         "shared with the DAW products, and a fix to one benefits both.", ""]
    for title, rows in grouped(found):
        L.append(f"### {title}")
        L.append("")
        for rel, classes in rows:
            for c in classes:
                meths = " · ".join(f"`{m}`" for m in c["methods"])
                identity = c.get("qualified_name", c["class"])
                use_as = identity
                template = c.get("template")
                if template is not None:
                    parameters = [part.strip() for part in template.split(",")]
                    if (len(parameters) == 1
                            and re.match(r"^(?:typename|class)\b", parameters[0])):
                        use_as += "<float>"
                    elif all("=" in part for part in parameters):
                        use_as += "<>"
                L.append(f"- `#include <pulp/signal/{rel}>` → **`{use_as}`**")
                if c.get("template") is not None:
                    L.append(f"  - template: `template <{c['template']}>`")
                if meths:
                    L.append(f"  - {meths}")
                if c.get("behavior_contract"):
                    L.append(f"  - behavior contract: {c['behavior_contract']}")
                for enum in c.get("associated_enums", []):
                    values = " · ".join(
                        f"`{enum['name']}::{value}`" for value in enum["values"])
                    L.append(f"  - associated enum: {values}")
                for ename, evals in c.get("enums", []):
                    joined = " · ".join(f"`{v}`" for v in evals)
                    L.append(f"  - `{ename}`: {joined}")
        L.append("")
    return "\n".join(L)


if __name__ == "__main__":
    f = scan()
    if "--query" in sys.argv:
        index = sys.argv.index("--query")
        if index + 1 >= len(sys.argv):
            raise SystemExit("--query requires the module request")
        f = shortlist(f, sys.argv[index + 1])
    if "--json" in sys.argv:
        print(json.dumps(f, indent=2))
    else:
        n = sum(len(v) for v in f.values())
        print(markdown(f))
        print(f"<!-- {n} classes across {len(f)} headers, extracted from core/signal -->",
              file=sys.stderr)
