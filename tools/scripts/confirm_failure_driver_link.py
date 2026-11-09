#!/usr/bin/env python3
"""Decide whether confirm_failure.sh's test driver carries the edited code.

With --subject, confirm_failure.sh rebuilds only the subject, so a test driver
that also links the edited source (or includes the edited header) would run
the unbroken copy and could pass with the fix broken. This answers the question
from the build's own graph, never from symbol names: inline functions and
template instantiations from a shared header are emitted into every object
that uses them, and Mach-O nm prints those weak definitions as strong "T", so a
symbol comparison sees a link that does not exist.

A source edit reaches a driver when the walk over its link inputs (its
objects, and recursively the objects of every library it links) reaches the
object compiled from the edited file. A header edit reaches it when one of
those objects depends on the header in ninja's dependency log. A link is
harmless when --target itself rebuilds the driver.

Every word of --test that is a file under the build dir is a driver, except
the subject, which --target rebuilds. A --test with no such word (a script,
ctest, a command on PATH) may run a binary that links the edited code, so it
cannot be checked.

Exit status, read by confirm_failure.sh:
  0  a driver carries the edited code; the linking object is printed
  1  no driver does; "rebuilt-by-target" is printed when one does but
     --target rebuilds it
  2  the graph cannot answer; the reason is printed. Any crash exits 2 too.

CMake-layout assumptions. None of these is a documented CMake interface, so
re-check each one when the pinned CMake moves:
  Ninja generator
    - `ninja -t query <node>` prints "<node>:", then "  input: <rule>" with
      inputs indented four spaces; implicit inputs start "| ", order-only
      ones "|| ", validations "|@ ".
    - `ninja -t deps <node>` prints "<node>: #deps N, deps mtime ... (VALID|
      STALE)" followed by indented paths, and keeps a stale record for an
      object that is currently missing.
    - Static libraries are implicit inputs of the link edge; objects are
      explicit inputs.
  Unix Makefiles generator
    - Each target's link commands are in CMakeFiles/<t>.dir/link.txt, run
      from the directory that holds that CMakeFiles; the output follows
      "-o", an archive follows the flags argument of `ar`, response files
      appear as "@<path>" relative to that directory, and -Wl, arguments are
      comma-joined.
    - CMakeFiles/<t>.dir/build.make has "<obj>: <source>" lines, paths
      relative to the build top.
    - CMakeFiles/Makefile2 has "<t>: <dir>/rule" for each target (written
      "<t> : <dir>/rule" when the name is one character), "all: <dir>/all"
      for the default target, and "<dir>/all: <dep>/all" dependency edges.
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from typing import Callable, Iterable

LIBRARY = re.compile(r"\.(a|lib|dylib|so(\.[0-9]+)*)$")
OBJECT_SUFFIXES = (".o", ".obj")


class Unknown(Exception):
    """The graph cannot answer; the message says why."""


def realpath(path: str, base: str | None = None) -> str:
    if base is not None and not os.path.isabs(path):
        path = os.path.join(base, path)
    return os.path.realpath(path)


def inside(path: str, root: str) -> bool:
    return os.path.commonpath([path, root]) == root


def is_object(path: str) -> bool:
    return path.endswith(OBJECT_SUFFIXES)


# ── Text parsers (pure; the unit tests feed them captured output) ───────────


def parse_ninja_query(text: str) -> dict[str, list[str]]:
    """Explicit and implicit inputs of each node in `ninja -t query` output.

    Order-only inputs sequence the build without being linked, and
    validations are not inputs at all, so neither is returned.
    """
    inputs: dict[str, list[str]] = {}
    node = section = None
    for line in text.splitlines():
        if line and not line.startswith(" ") and line.endswith(":"):
            node, section = line[:-1], None
            inputs[node] = []
        elif line.startswith("  ") and not line.startswith("    "):
            section = line.strip().split(":", 1)[0]
        elif section == "input" and node is not None and line.strip():
            item = line.strip()
            if item.startswith("||") or item.startswith("|@"):
                continue
            if item.startswith("| "):
                item = item[2:]
            inputs[node].append(item)
    return inputs


def parse_ninja_deps(text: str) -> dict[str, list[str] | None]:
    """Each node's recorded dependencies from `ninja -t deps`; None when the
    log has no record for it."""
    deps: dict[str, list[str] | None] = {}
    node = None
    for line in text.splitlines():
        if line and not line.startswith(" "):
            node = line.split(":", 1)[0]
            deps[node] = [] if "#deps" in line else None
        elif line.strip() and node is not None:
            recorded = deps.get(node)
            if recorded is not None:
                recorded.append(line.strip())
    return deps


def parse_ninja_inputs(text: str) -> list[str]:
    """`ninja -t inputs` prints one node per line; a path may hold spaces."""
    return [line.strip() for line in text.splitlines() if line.strip()]


def parse_link_txt(text: str, read_rsp: Callable[[str], str] | None = None
                   ) -> tuple[set[str], list[str]]:
    """Outputs and object/library inputs of one link.txt, paths as written.

    read_rsp returns a response file's text for "@<path>", or raises OSError.
    """
    outputs: set[str] = set()
    inputs: list[str] = []
    for line in text.splitlines():
        try:
            argv = shlex.split(line)
        except ValueError:
            continue
        if not argv:
            continue
        tokens: list[str] = []
        for arg in argv[1:]:
            if arg.startswith("@") and read_rsp is not None:
                try:
                    tokens += shlex.split(read_rsp(arg[1:]))
                except (OSError, ValueError):
                    pass
            elif arg.startswith("-Wl,"):
                tokens += arg.split(",")[1:]
            else:
                tokens.append(arg)
        if "-o" in tokens and tokens.index("-o") + 1 < len(tokens):
            outputs.add(tokens[tokens.index("-o") + 1])
        elif os.path.basename(argv[0]).endswith("ar") and len(tokens) >= 2:
            outputs.add(tokens[1])  # ar <flags> <archive> <objects...>
        inputs += [t for t in tokens if is_object(t) or LIBRARY.search(t)]
    return outputs, [i for i in inputs if i not in outputs]


def parse_makefile2(text: str) -> tuple[dict[str, set[str]], dict[str, set[str]]]:
    """Target entry points and the .dir dependency edges of CMakeFiles/Makefile2.

    Returns ({target: {dir, ...}}, {dir: {dependency dir, ...}}). A target's
    entry is its own "<dir>/rule", or for `all` each "<dir>/all" it lists.
    """
    starts: dict[str, set[str]] = {}
    edges: dict[str, set[str]] = {}
    for line in text.splitlines():
        if not line or line.startswith(("\t", "#", " ")) or ":" not in line:
            continue
        lhs, rhs = (part.strip() for part in line.split(":", 1))
        if not lhs or not rhs or " " in rhs:
            continue
        if lhs.endswith("/all") and rhs.endswith("/all"):
            edges.setdefault(lhs[: -len("/all")], set()).add(rhs[: -len("/all")])
        elif "/" not in lhs:
            for suffix in ("/rule", "/all"):
                if rhs.endswith(suffix):
                    starts.setdefault(lhs, set()).add(rhs[: -len(suffix)])
    return starts, edges


def makefile2_closure(starts: dict[str, set[str]], edges: dict[str, set[str]],
                      target: str) -> set[str]:
    """Every .dir that building `target` builds; empty when it is unknown."""
    seen = set(starts.get(target, ()))
    frontier = list(seen)
    while frontier:
        nxt = [d for node in frontier for d in edges.get(node, ()) if d not in seen]
        seen.update(nxt)
        frontier = nxt
    return seen


def parse_build_make(text: str) -> list[tuple[str, str]]:
    """The "<obj>: <dependency>" rule lines of a target's build.make."""
    rules = []
    for line in text.splitlines():
        if line.startswith(("\t", "#")):
            continue
        lhs, sep, rhs = line.partition(": ")
        if sep and lhs.strip() and rhs.strip():
            rules.append((lhs.strip(), rhs.strip()))
    return rules


def pick_drivers(test_cmd: str, build: str, subject: str,
                 cwd: str | None = None) -> list[str]:
    """Every build-dir file named by --test, minus the subject.

    Raises Unknown when --test names no build-dir file, since whatever it
    runs cannot be traced to the graph.
    """
    try:
        words = shlex.split(test_cmd)
    except ValueError as exc:
        raise Unknown(f"--test cannot be parsed into words ({exc})") from exc
    candidates: list[str] = []
    for word in words:
        path = realpath(word, cwd)
        if os.path.isfile(path) and inside(path, build) and path not in candidates:
            candidates.append(path)
    if not candidates:
        first = words[0] if words else ""
        raise Unknown(
            f"no word of --test is a file under {build} (the first word is "
            f"'{first}'), so the binary it runs cannot be checked for a copy of "
            "the edited code; name the driver binary itself in --test")
    subject = realpath(subject, cwd)
    return [c for c in candidates if c != subject]


@dataclass
class Edit:
    """What was edited, and how to recognise the object that carries it."""
    file: str            # the edited file, real path
    obj_name: str        # basename of the object to look for, e.g. value.cpp.o
    object_source: str | None = None  # --object given as an existing source path
    by_object_name: bool = False      # --object given as a bare name
    is_header: bool = False

    def carried_by(self, obj: str, sources: Iterable[str]) -> bool:
        """Whether `obj`, compiled from `sources` (real paths), carries the edit."""
        if os.path.basename(obj) != self.obj_name:
            return False
        sources = list(sources)
        if self.object_source is not None:
            return self.object_source in sources
        if self.by_object_name:
            # A bare --object name has no identity beyond its basename, so any
            # object of that name counts: refusing is the safe direction.
            return True
        return self.file in sources


# ── Ninja lane ──────────────────────────────────────────────────────────────


@dataclass
class NinjaGraph:
    build: str
    ninja: str
    run: Callable[[list[str]], subprocess.CompletedProcess] = field(
        default=lambda argv: subprocess.run(argv, capture_output=True, text=True))

    def tool(self, name: str, nodes: list[str]) -> str | None:
        proc = self.run([self.ninja, "-C", self.build, "-t", name, *nodes])
        return proc.stdout if proc.returncode == 0 else None

    def query(self, nodes: list[str]) -> dict[str, list[str]] | None:
        out = self.tool("query", nodes)
        return None if out is None else parse_ninja_query(out)

    def node_path(self, node: str) -> str:
        return realpath(node, self.build)

    def objects(self, driver: str) -> list[str]:
        driver_node = os.path.relpath(driver, self.build)
        graph = self.query([driver_node])
        if graph is None or not graph.get(driver_node):
            raise Unknown(f"ninja does not know how {driver_node} is linked")
        seen, frontier, objects = {driver_node}, [driver_node], []
        while frontier:
            nxt = []
            for node in frontier:
                for item in graph.get(node, []):
                    if item in seen:
                        continue
                    seen.add(item)
                    if is_object(item):
                        objects.append(item)
                    elif LIBRARY.search(item):
                        nxt.append(item)
            if nxt:
                more = self.query(nxt)
                if more is None:
                    raise Unknown("ninja could not describe a library the driver "
                                  "links: " + " ".join(nxt))
                graph.update(more)
            frontier = nxt
        if not objects:
            raise Unknown(f"{driver_node} is not linked from any object (a custom "
                          "command output, for example), so what it runs is unknown")
        return objects

    def carrying_object(self, edit: Edit, objects: list[str]) -> str | None:
        if edit.is_header:
            out = self.tool("deps", objects)
            if out is None:
                raise Unknown("`ninja -t deps` failed, so which headers the "
                              "driver's objects include is unknown")
            deps = parse_ninja_deps(out)
            for obj in objects:
                recorded = deps.get(obj)
                if recorded is None:
                    raise Unknown(f"ninja's dependency log has no record for {obj}, "
                                  "so whether it includes the edited header is unknown")
                if any(self.node_path(d) == edit.file for d in recorded):
                    return obj
            return None
        candidates = [o for o in objects if os.path.basename(o) == edit.obj_name]
        if not candidates:
            return None
        sources = self.query(candidates)
        if sources is None:
            raise Unknown("ninja could not describe " + " ".join(candidates))
        for obj in candidates:
            if edit.carried_by(obj, (self.node_path(s) for s in sources.get(obj, []))):
                return obj
        return None

    def target_rebuilds(self, target: str, driver: str) -> bool:
        out = self.tool("inputs", [target])
        if out is None:
            return False
        nodes = {target, *parse_ninja_inputs(out)}
        return any(self.node_path(n) == driver for n in nodes)


# ── Makefile lane ───────────────────────────────────────────────────────────


@dataclass
class MakeLink:
    target_dir: str      # the CMakeFiles/<t>.dir that holds link.txt
    inputs: list[str]    # real paths


class MakeGraph:
    """Every link.txt in the build dir, read once."""

    def __init__(self, build: str, stash: str = ""):
        self.build = build
        self.links: dict[str, MakeLink] = {}
        for root, dirs, files in os.walk(build):
            if stash and inside(os.path.realpath(root), stash):
                dirs[:] = []
                continue
            if "link.txt" not in files or not root.endswith(".dir"):
                continue
            cwd = os.path.dirname(os.path.dirname(root))  # parent of CMakeFiles

            def read_rsp(path: str, cwd: str = cwd) -> str:
                with open(os.path.join(cwd, path)) as rsp:
                    return rsp.read()

            with open(os.path.join(root, "link.txt")) as stream:
                outputs, inputs = parse_link_txt(stream.read(), read_rsp)
            for out in outputs:
                self.links[realpath(out, cwd)] = MakeLink(
                    root, [realpath(i, cwd) for i in inputs])
        self._makefile2: tuple[dict, dict] | None = None

    def objects(self, driver: str) -> list[str]:
        if driver not in self.links:
            raise Unknown(f"no link.txt in {self.build} produces {driver}")
        seen, frontier, objects = {driver}, [driver], []
        while frontier:
            nxt = []
            for node in frontier:
                link = self.links.get(node)
                for item in link.inputs if link else ():
                    if item in seen:
                        continue
                    seen.add(item)
                    if is_object(item):
                        objects.append(item)
                    elif item in self.links:
                        nxt.append(item)
            frontier = nxt
        if not objects:
            raise Unknown(f"no object is linked into {driver}, so what it runs is unknown")
        return objects

    def sources_of(self, obj: str) -> list[str]:
        """The sources build.make names for `obj`; raises Unknown when absent."""
        obj_dir = os.path.dirname(obj)
        while obj_dir != self.build and not obj_dir.endswith(".dir"):
            parent = os.path.dirname(obj_dir)
            if parent == obj_dir:
                break
            obj_dir = parent
        try:
            with open(os.path.join(obj_dir, "build.make")) as stream:
                rules = parse_build_make(stream.read())
        except OSError as exc:
            raise Unknown(f"no build.make names the source of {obj}") from exc
        return [realpath(rhs, self.build) for lhs, rhs in rules
                if realpath(lhs, self.build) == obj]

    def carrying_object(self, edit: Edit, objects: list[str]) -> str | None:
        if edit.is_header:
            raise Unknown("the Makefile generator keeps no dependency log to say "
                          "whether the driver includes the edited header")
        for obj in objects:
            if os.path.basename(obj) == edit.obj_name and edit.carried_by(
                    obj, self.sources_of(obj)):
                return obj
        return None

    def target_rebuilds(self, target: str, driver: str) -> bool:
        if self._makefile2 is None:
            try:
                with open(os.path.join(self.build, "CMakeFiles", "Makefile2")) as stream:
                    self._makefile2 = parse_makefile2(stream.read())
            except OSError:
                return False
        starts, edges = self._makefile2
        driver_dir = os.path.realpath(self.links[driver].target_dir)
        return any(realpath(d, self.build) == driver_dir
                   for d in makefile2_closure(starts, edges, target))


# ── Decision ────────────────────────────────────────────────────────────────


def ninja_program(build: str) -> str | None:
    override = os.environ.get("CONFIRM_FAILURE_NINJA")
    if override is not None:
        return override if os.access(override, os.X_OK) else None
    try:
        with open(os.path.join(build, "CMakeCache.txt")) as cache:
            for line in cache:
                if line.startswith("CMAKE_MAKE_PROGRAM:"):
                    candidate = line.split("=", 1)[1].strip()
                    if os.access(candidate, os.X_OK):
                        return candidate
    except OSError:
        pass
    return shutil.which("ninja")


def decide(build: str, edit: Edit, test_cmd: str, target: str, subject: str,
           stash: str = "") -> tuple[int, str]:
    drivers = pick_drivers(test_cmd, build, subject)
    if not drivers:
        return 1, ""  # the subject is the only driver, and --target rebuilds it
    if os.path.exists(os.path.join(build, "build.ninja")):
        ninja = ninja_program(build)
        if ninja is None:
            raise Unknown("the build dir uses Ninja but no ninja executable was found")
        graph: NinjaGraph | MakeGraph = NinjaGraph(build, ninja)
    elif os.path.exists(os.path.join(build, "Makefile")):
        if edit.is_header:
            raise Unknown("the Makefile generator keeps no dependency log to say "
                          "whether the driver includes the edited header")
        graph = MakeGraph(build, stash)
    else:
        raise Unknown(f"{build} has neither build.ninja nor a Makefile to read the "
                      "link graph from")
    rebuilt = False
    for driver in drivers:
        carrying = graph.carrying_object(edit, graph.objects(driver))
        if carrying is None:
            continue
        if graph.target_rebuilds(target, driver):
            rebuilt = True
            continue
        return 0, f"{carrying} in {driver}"
    return 1, "rebuilt-by-target" if rebuilt else ""


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--file", required=True)
    parser.add_argument("--obj-name", required=True)
    parser.add_argument("--object", default="")
    parser.add_argument("--test", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--subject", required=True)
    parser.add_argument("--stash", default="")
    parser.add_argument("--header", choices=("0", "1"), default="0",
                        help="1 when the edited file is a header")
    args = parser.parse_args(argv)
    try:
        build = os.path.realpath(args.build_dir)
        object_source = None
        if args.object and os.path.isfile(args.object):
            object_source = os.path.realpath(args.object)
        edit = Edit(file=os.path.realpath(args.file), obj_name=args.obj_name,
                    object_source=object_source,
                    by_object_name=bool(args.object) and object_source is None,
                    is_header=args.header == "1")
        status, message = decide(build, edit, args.test, args.target, args.subject,
                                 os.path.realpath(args.stash) if args.stash else "")
    except Unknown as exc:
        print(exc)
        return 2
    except Exception as exc:  # a crash must never read as "does not link"
        print(f"the link check failed: {type(exc).__name__}: {exc}")
        return 2
    if message:
        print(message)
    return status


if __name__ == "__main__":
    sys.exit(main())
