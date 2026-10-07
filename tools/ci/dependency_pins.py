#!/usr/bin/env python3
"""Which dependencies a change to the pin files moved, on one platform.

The pin files are tools/deps/manifest.json, tools/cmake/PulpDependencies.cmake
and tools/cmake/PulpFetchContent.cmake. A change to one of them can move a
dependency's content without moving any path the codemodel digests, so the
key code treats the executables that build against a moved dependency as
`dependency_pin`. This module says which dependencies moved, or that the
change cannot be attributed and every executable must be treated so.

Attribution is by dependency name (the manifest's `name`):

  manifest.json      entries are compared by name. Documentation fields
                     (DOC_FIELDS) do not count. A Skia/V8-style
                     `determinism.release_assets.<key>` change counts only
                     on the platform the key names.
  PulpDependencies   the file is split into top-level blocks (a comment
                     header after a blank line, outside any command or
                     control block). A block belongs to the names whose
                     anchor matches its code. Blocks are compared on their
                     platform-effective commands: comments and layout
                     dropped, and an if/elseif/else chain reduced to the
                     branch this platform takes when every condition up to
                     it is a bare platform variable (PLATFORM_TRUTH).
  PulpFetchContent   shared logic: any platform-effective change moves all.

Everything that cannot be attributed moves all (`scope` "all"): a file that
does not parse, a changed block no anchor claims, blocks added, removed or
reordered without an anchor, and a changed name the map does not know.
Whether a known name reaches an executable is the closure's question
(`DependencyIndex`), which also refuses a name it cannot see on this
platform.
"""
from __future__ import annotations

import fnmatch
import json
import re
from dataclasses import dataclass, field
from pathlib import Path

MAP_SCHEMA = "pulp-dependency-pin-map/v1"
MAP_PATH = Path(__file__).resolve().parent / "dependency_pin_map.json"
MANIFEST = "tools/deps/manifest.json"
DEPENDENCIES_CMAKE = "tools/cmake/PulpDependencies.cmake"
FETCHCONTENT_CMAKE = "tools/cmake/PulpFetchContent.cmake"
PIN_PATHS = (MANIFEST, DEPENDENCIES_CMAKE, FETCHCONTENT_CMAKE)
# Manifest fields no build reads: inventory, licensing and audit metadata.
DOC_FIELDS = frozenset({"notes", "license", "category", "repository", "upstream", "documented_in_dependencies_md",
                        "documented_in_notice_md", "source_files", "external_names"})
# A release asset key's platform, by prefix; an unknown key counts everywhere.
ASSET_PLATFORMS = (("mac", "darwin"), ("darwin", "darwin"), ("linux", "linux"), ("win", "windows"),
                   ("ios", "ios"), ("visionos", "ios"), ("android", "android"), ("wasm", "wasm"))
# CMake's platform variables on a host build of each platform. Only these
# decide a branch; any other condition keeps the whole chain.
PLATFORM_TRUTH = {
    "darwin": {"APPLE": True, "UNIX": True, "WIN32": False, "ANDROID": False, "EMSCRIPTEN": False,
               "IOS": False, "MSVC": False},
    "linux": {"APPLE": False, "UNIX": True, "WIN32": False, "ANDROID": False, "EMSCRIPTEN": False,
              "IOS": False, "MSVC": False},
}
_OPENERS = {"if": "endif", "foreach": "endforeach", "while": "endwhile", "function": "endfunction",
            "macro": "endmacro", "block": "endblock"}
_CLOSERS = frozenset(_OPENERS.values())


class Unattributable(Exception):
    """The change moves every dependency; the message says why."""


@dataclass(frozen=True)
class Pins:
    """`scope` "all" (every executable) or "names" (those reaching `names`)."""
    scope: str
    names: frozenset[str] = frozenset()
    why: str | None = None

    def as_json(self) -> dict:
        return {"scope": self.scope, "names": sorted(self.names), "why": self.why}


NONE = Pins("names")


def load_map(path: Path = MAP_PATH) -> dict:
    doc = json.loads(path.read_text(encoding="utf-8"))
    if doc.get("schema") != MAP_SCHEMA or not isinstance(doc.get("dependencies"), dict):
        raise ValueError(f"{path} is not a {MAP_SCHEMA} document")
    return doc["dependencies"]


# -- manifest -----------------------------------------------------------------

def _asset_platform(key: str) -> str | None:
    return next((plat for prefix, plat in ASSET_PLATFORMS if key.startswith(prefix)), None)


def _manifest_entries(text: str | None) -> dict[str, dict]:
    if text is None:
        raise Unattributable(f"{MANIFEST} is absent on one side")
    try:
        doc = json.loads(text)
    except ValueError as error:
        raise Unattributable(f"{MANIFEST} does not parse ({error.__class__.__name__})") from None
    if not isinstance(doc, dict) or set(doc) != {"dependencies"} or not isinstance(doc["dependencies"], list):
        raise Unattributable(f"{MANIFEST} has fields beyond its dependency list")
    out: dict[str, dict] = {}
    for entry in doc["dependencies"]:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str) or entry["name"] in out:
            raise Unattributable(f"{MANIFEST} has an unnamed or repeated entry")
        out[entry["name"]] = entry
    return out


def manifest_names(base: str | None, head: str | None, platform: str) -> set[str]:
    b, h = _manifest_entries(base), _manifest_entries(head)
    moved: set[str] = set()
    for name in set(b) | set(h):
        be, he = b.get(name), h.get(name)
        if be == he:
            continue
        if be is None or he is None:
            moved.add(name)
            continue
        for key in (set(be) | set(he)) - DOC_FIELDS:
            if be.get(key) == he.get(key):
                continue
            bd, hd = be.get(key), he.get(key)
            if key == "determinism" and isinstance(bd, dict) and isinstance(hd, dict):
                for part in set(bd) | set(hd):
                    if bd.get(part) == hd.get(part):
                        continue
                    ba, ha = bd.get(part), hd.get(part)
                    if part == "release_assets" and isinstance(ba, dict) and isinstance(ha, dict):
                        if any(ba.get(a) != ha.get(a) and _asset_platform(a) in (platform, None)
                               for a in set(ba) | set(ha)):
                            moved.add(name)
                    else:
                        moved.add(name)
            else:
                moved.add(name)
    return moved


# -- CMake ----------------------------------------------------------------------

@dataclass
class Command:
    name: str           # lower-cased command name
    text: str           # normalized: one space between arguments, strings verbatim
    line: int           # 1-based line the command starts on


def commands(text: str, path: str) -> tuple[list[Command], set[int]]:
    """The file's commands, and the 1-based lines that hold only a comment.
    Bracket arguments and bracket comments are refused rather than parsed."""
    cmds: list[Command] = []
    comment_lines: set[int] = set()
    i, line, n = 0, 1, len(text)
    line_has_code = False
    while i < n:
        c = text[i]
        if c == "\n":
            line, line_has_code = line + 1, False
            i += 1
        elif c in " \t\r":
            i += 1
        elif c == "#":
            if text.startswith("#[", i) and re.match(r"#\[=*\[", text[i:]):
                raise Unattributable(f"{path} uses a bracket comment")
            if not line_has_code:
                comment_lines.add(line)
            while i < n and text[i] != "\n":
                i += 1
        else:
            m = re.compile(r"[A-Za-z_][A-Za-z0-9_]*").match(text, i)
            if not m:
                raise Unattributable(f"{path} line {line} is not a command")
            start_line, name = line, m.group(0)
            i = m.end()
            while i < n and text[i] in " \t":
                i += 1
            if i >= n or text[i] != "(":
                raise Unattributable(f"{path} line {line} is not a command")
            args, depth, i = [], 0, i
            token = ""
            while True:
                if i >= n:
                    raise Unattributable(f"{path} has an unterminated command at line {start_line}")
                c = text[i]
                if c == '"':
                    j = i + 1
                    while j < n and text[j] != '"':
                        if text[j] == "\\":
                            j += 1
                        elif text[j] == "\n":
                            line += 1
                        j += 1
                    if j >= n:
                        raise Unattributable(f"{path} has an unterminated string at line {start_line}")
                    token += text[i:j + 1]
                    i = j + 1
                    continue
                if c == "#":
                    if re.match(r"#\[=*\[", text[i:]):
                        raise Unattributable(f"{path} uses a bracket comment")
                    while i < n and text[i] != "\n":
                        i += 1
                    continue
                if c == "[" and re.match(r"\[=*\[", text[i:]):
                    raise Unattributable(f"{path} uses a bracket argument")
                if c in " \t\r\n":
                    if c == "\n":
                        line += 1
                    if token:
                        args.append(token)
                        token = ""
                    i += 1
                    continue
                if c == "(":
                    depth += 1
                elif c == ")":
                    depth -= 1
                    if depth == 0:
                        if token:
                            args.append(token)
                        i += 1
                        break
                token += c
                i += 1
            inner = " ".join(args)[1:].strip()  # drop the opening parenthesis
            cmds.append(Command(name.lower(), f"{name.lower()}({inner})", start_line))
            line_has_code = True
    return cmds, comment_lines


def blocks(text: str, path: str) -> list[list[Command]]:
    """Top-level blocks: a comment line after a blank line (or the file
    start), outside any command and any control block, opens one."""
    cmds, comment_lines = commands(text, path)
    lines = text.split("\n")
    starts = []
    depth, k = 0, 0
    for number in range(1, len(lines) + 1):
        while k < len(cmds) and cmds[k].line < number:
            depth += (cmds[k].name in _OPENERS) - (cmds[k].name in _CLOSERS)
            k += 1
        previous_blank = number == 1 or not lines[number - 2].strip()
        if number in comment_lines and previous_blank and depth == 0:
            starts.append(number)
    out: list[list[Command]] = [[]]
    bounds = iter(starts)
    nxt = next(bounds, None)
    for cmd in cmds:
        while nxt is not None and cmd.line >= nxt:
            out.append([])
            nxt = next(bounds, None)
        out[-1].append(cmd)
    return [b for b in out if b]


_BARE = re.compile(r"^(?:if|elseif)\((NOT )?([A-Z0-9_]+)\)$")


def effective(cmds: list[Command], platform: str) -> list[str]:
    """The commands this platform runs, as text: a chain whose conditions up
    to the taken branch are all decidable becomes that branch's body; any
    other chain stays, with its bodies reduced the same way."""
    truth = PLATFORM_TRUTH.get(platform, {})
    out: list[str] = []
    i = 0
    while i < len(cmds):
        if cmds[i].name != "if":
            out.append(cmds[i].text)
            i += 1
            continue
        heads, bodies, depth, j = [cmds[i]], [[]], 1, i + 1
        while j < len(cmds):
            c = cmds[j]
            if c.name == "if":
                depth += 1
            elif c.name == "endif":
                depth -= 1
                if depth == 0:
                    break
            elif c.name in ("elseif", "else") and depth == 1:
                heads.append(c)
                bodies.append([])
                j += 1
                continue
            bodies[-1].append(c)
            j += 1
        taken = None
        for head, body in zip(heads, bodies):
            if head.name == "else":
                taken = body
                break
            m = _BARE.match(head.text)
            if not m or m.group(2) not in truth:
                taken = False
                break
            if truth[m.group(2)] != bool(m.group(1)):
                taken = body
                break
        if taken is False:
            for head, body in zip(heads, bodies):
                out.append(head.text)
                out.extend(effective(body, platform))
            out.append(cmds[j].text if j < len(cmds) else "endif()")
        else:
            out.extend(effective(taken or [], platform))
        i = j + 1
    return out


def anchors_of(dep_map: dict) -> dict[str, list[re.Pattern]]:
    """Each name's block anchors: its own, plus the FetchContent calls that
    name its FetchContent directory."""
    return {name: [re.compile(a) for a in entry.get("anchors") or []]
            + [re.compile(r"\b(?:pulp_register_fetchcontent_source|fetchcontent_declare|fetchcontent_makeavailable)"
                          r"\((?i:" + re.escape(d) + r")(?=[\s)])") for d in entry.get("fetchcontent") or []]
            for name, entry in dep_map.items()}


def owners(block: list[Command], anchors: dict[str, list[re.Pattern]]) -> frozenset[str]:
    code = "\n".join(c.text for c in block)
    return frozenset(name for name, pats in anchors.items() if any(p.search(code) for p in pats))


def cmake_names(base: str | None, head: str | None, platform: str, dep_map: dict) -> set[str]:
    anchors = anchors_of(dep_map)
    sides = []
    for text in (base, head):
        if text is None:
            raise Unattributable(f"{DEPENDENCIES_CMAKE} is absent on one side")
        rows = [(owners(b, anchors), effective(b, platform)) for b in blocks(text, DEPENDENCIES_CMAKE)]
        sides.append(rows)
    shared = [[body for names, body in rows if not names] for rows in sides]
    if shared[0] != shared[1]:
        raise Unattributable(f"{DEPENDENCIES_CMAKE} changed outside any dependency's block")
    moved: set[str] = set()
    owned: list[dict] = [{} for _ in sides]
    for side, rows in zip(owned, sides):
        for names, body in rows:
            if names:
                side.setdefault(names, []).append(body)
    for names in set(owned[0]) | set(owned[1]):
        if owned[0].get(names) != owned[1].get(names):
            moved |= names
    return moved


def attribute(base: dict[str, str | None], head: dict[str, str | None], platform: str,
              dep_map: dict) -> Pins:
    """Pins moved between two copies of the pin files, keyed by path; a path
    absent from both dicts is unchanged."""
    try:
        moved: set[str] = set()
        for path in PIN_PATHS:
            if path not in base and path not in head:
                continue
            b, h = base.get(path), head.get(path)
            if b == h:
                continue
            if path == MANIFEST:
                moved |= manifest_names(b, h, platform)
            elif path == DEPENDENCIES_CMAKE:
                moved |= cmake_names(b, h, platform, dep_map)
            else:
                eb = effective(commands(b, path)[0], platform) if b is not None else None
                eh = effective(commands(h, path)[0], platform) if h is not None else None
                if eb != eh:
                    raise Unattributable(f"{path} is shared logic")
        unknown = sorted(n for n in moved if n not in dep_map)
        if unknown:
            raise Unattributable(f"no target mapping for {', '.join(unknown)}")
        return Pins("names", frozenset(moved))
    except Unattributable as error:
        return Pins("all", why=str(error))


# -- closure ---------------------------------------------------------------------

@dataclass
class DependencyIndex:
    """Which executables reach which dependency on this platform: through
    the codemodel targets a dependency builds (derived from its FetchContent
    build directory, or named) or the archives a recorded link pulled."""
    dep_map: dict
    platform: str
    head_targets: dict
    base_targets: dict
    links: dict | None
    _closure: dict = field(default_factory=dict)

    def targets_of(self, name: str) -> set[str]:
        entry = self.dep_map[name]
        prefixes = tuple(f"<build>/_deps/{d}-build/" for d in entry.get("fetchcontent") or [])
        found = set()
        for side in (self.head_targets, self.base_targets):
            for target, t in side.items():
                if target in (entry.get("targets") or []) or (
                        prefixes and any(a.startswith(prefixes) for a in t.get("artifacts") or [])):
                    found.add(target)
        return found

    def _deps(self, target: str) -> set[str]:
        if target not in self._closure:
            self._closure[target] = set()  # cycles terminate
            seen: set[str] = set()
            for side in (self.head_targets, self.base_targets):
                for dep in (side.get(target) or {}).get("dependencies") or []:
                    seen |= {dep} | self._deps(dep)
            self._closure[target] = seen
        return self._closure[target]

    def _archives(self, artifact: str) -> list[str]:
        rec = (self.links or {}).get("<build>/" + artifact) or {}
        return list(rec.get("archives") or {}) + list(rec.get("shared") or [])

    def builds_here(self, name: str) -> bool:
        plats = self.dep_map[name].get("platforms")
        return plats is None or self.platform in plats

    def visible(self, name: str) -> bool:
        """The positive control: the map finds the dependency in this
        platform's build (a target, or a recorded archive)."""
        if self.targets_of(name):
            return True
        globs = self.dep_map[name].get("archives") or []
        return any(fnmatch.fnmatchcase(a, g) for rec in (self.links or {}).values()
                   for a in list(rec.get("archives") or {}) + list(rec.get("shared") or []) for g in globs)

    def reaches(self, artifact: str, target: str, names: frozenset[str]) -> bool:
        closure = self._deps(target) | {target}
        archives = self._archives(artifact)
        for name in names:
            if closure & self.targets_of(name):
                return True
            if any(fnmatch.fnmatchcase(a, g) for a in archives for g in self.dep_map[name].get("archives") or []):
                return True
        return False


def resolve(pins: Pins, index: DependencyIndex) -> Pins:
    """Drop names not built on this platform, and widen to all when a name
    that is built here cannot be seen in the build."""
    if pins.scope == "all":
        return pins
    here = frozenset(n for n in pins.names if index.builds_here(n))
    blind = sorted(n for n in here if not index.visible(n))
    if blind:
        return Pins("all", why=f"the map finds no target or archive for {', '.join(blind)}")
    return Pins("names", here)
