#!/usr/bin/env python3
"""Enforce the timeline engine modules' dependency floors.

Upper adapter/host/UI/render layers may consume the engine, but engine modules
may include and link only their explicitly declared lower-layer closure.
"""

from __future__ import annotations

import argparse
import re
import shutil
import tempfile
from pathlib import Path


MODULE_FLOORS = {
    "timebase": {"timebase", "platform", "runtime"},
    # The document model. `timeline_editor` is absent, and that absence is load
    # bearing rather than incidental: it is what stops a reducer, a migration, or
    # a serializer from reaching for the editor's gesture verbs. A document model
    # that can name Draw/Erase/Move/Resize hands them to every consumer of the
    # model — a headless importer, a .pulpgraph loader, a plugin that wants only
    # commands — with no gate able to object. `music` is a lower, header-only
    # value vocabulary: admitting it lets the document model share checked
    # pitch/chord/scale identities without acquiring MIDI, signal, or playback.
    "timeline": {"timeline", "music", "timebase", "platform", "runtime"},
    # Durable project-package publication sits above the document model. It may
    # validate package-relative Timeline paths and use runtime/platform durability
    # primitives, but ZIP formats, interchange adapters, playback, and UI remain
    # outside this layer. Both self spellings are admitted because the directory
    # uses an underscore while the CMake alias uses a hyphen.
    "project_package": {
        "project_package",
        "project-package",
        "timeline",
        "music",
        "timebase",
        "platform",
        "runtime",
    },
    # Agent-facing read projections sit directly above the immutable document.
    # They may hash and summarize timeline values but must not acquire editor,
    # playback, interchange, UI, host, or adapter dependencies.
    "timeline_agent_view": {
        "timeline_agent_view",
        "timeline-agent-view",
        "timeline",
        "music",
        "timebase",
        "platform",
        "runtime",
    },
    # Interchange sits above the document model, not inside it: it may read a
    # document and consult what formats declare, but a format adapter, a plugin
    # host, or a view must never become something it can reach for.
    "interchange": {
        "interchange",
        "timeline",
        "music",
        "timebase",
        "platform",
        "runtime",
    },
    "playback": {
        "playback",
        "timeline",
        "music",
        "timebase",
        "audio",
        "midi",
        "platform",
        "runtime",
    },
    # The editor rung reaches the document model and the timebase and stops
    # there. Playback is absent on purpose: an editor view learns where the
    # playhead is through the SequencerUiHost interface declared here, which the
    # product shell implements, so a plugin that draws a piano roll over its own
    # engine consumes the editor without acquiring a transport. Both spellings
    # of the module's own name are allowed because the CMake target is
    # pulp::timeline-editor while the directory is timeline_editor. A view
    # target above this rung takes its own row rather than widening this one:
    # admitting view here would make every consumer of the editor kernel pay
    # for the view stack.
    #
    # This row is a superset of the timeline row, so it is not what keeps edit
    # intents free of pointer types — that holds equally at either address, and
    # is therefore no reason to place them here. The asymmetry runs the other
    # way and lives in the timeline row above: only that row can reject the
    # document model reaching up for a gesture verb.
    "timeline_editor": {
        "timeline_editor",
        "timeline-editor",
        "timeline",
        "music",
        "timebase",
        "platform",
        "runtime",
    },
    # The arranger rung, above the editor kernel. It is the first thing that
    # consumes the kernel rather than declaring it, and it is where the view
    # and canvas stack becomes admissible — which is the whole reason it is a
    # row of its own instead of a widening of timeline_editor's.
    #
    # Two absences carry the ladder. `playback` keeps a view's only coupling
    # toward audio the SequencerUiHost interface, so an arranger drawn over
    # somebody else's engine acquires no transport. `project_package` keeps
    # storage a sibling rung rather than a base: an editor is proven against
    # serialize round-trip, and re-hosting it on a package protocol later is
    # adapter work above this row, not a change to it.
    "timeline_view": {
        "timeline_view",
        "timeline-view",
        "timeline_editor",
        "timeline-editor",
        "timeline",
        "music",
        "timebase",
        "view",
        "canvas",
        "platform",
        "runtime",
    },
}

# Modules a row's *link closure* drags in beyond its floor. These are recorded
# facts about the linked artifact, not endorsements, and they are deliberately
# kept out of MODULE_FLOORS: a floor row also governs which headers a module's
# sources may include, and nothing here has earned that. Widening the row itself
# would quietly grant `playback` the right to `#include <pulp/state/...>` — the
# opposite of what measuring the closure was for.
#
# An entry is a debt to be paid down, not a permission to build on. Deleting one
# once the underlying link is cut tightens the gate with no other edit.
LINK_CLOSURE_DEBT = {
    # core/playback links pulp::audio, which links pulp::state, pulp::signal and
    # pulp::sample-bank-manifest PUBLIC and, through pulp::state, pulp::events
    # PRIVATE. The distinction survives the trip and matters for a
    # pay-for-what-you-use claim:
    #   state, signal, sample-bank-manifest — PUBLIC, so their include
    #     directories propagate; a playback consumer genuinely can reach
    #     `#include <pulp/state/...>` today.
    #   events — PRIVATE, link-only; no header surface propagates.
    #   signal — an INTERFACE (header-only) library, so paying for it costs
    #     headers, not object code.
    # Whether playback should reach the state store at all is an open design
    # question this entry does not answer. It exists so the gate can police
    # whatever answer is reached, instead of passing green on a floor the
    # artifact never honoured.
    "playback": {"state", "signal", "sample_bank_manifest", "events"},
    "timeline_view": {
        "audio",
        "events",
        "midi",
        "render",
        "sample_bank_manifest",
        "signal",
        "state",
    },
    # core/timeline_view links the concrete pulp::view-core target for canvas
    # and input types. Its closure still carries the ordinary view substrate
    # recorded above, but host, playback, format, and graph are absent: the
    # host-backed GraphEditorView implementation now lives in core/host.
}

# Modules under core/ that link a declared engine module but carry no floor row
# of their own. Every entry needs a reason, and the reason is the point: this is
# a decision record, not a mute list. A module reaching the engine either earns a
# MODULE_FLOORS row or states here why it does not need one.
#
# verify_engine_consumers() rejects an entry with no reason and an entry that no
# longer links an engine module, so the list cannot quietly outlive its subject.
ENGINE_CONSUMERS = {
    "midi": "MIDI transformation utilities consume timebase for prepared tempo "
            "maps and sample-domain scheduling. They remain an event-processing "
            "layer, not a rung in the timeline document/playback ladder.",
    "signal": "Header-only DSP layer. It consumes timebase only for the canonical "
              "persisted beat-division vocabulary; this narrow value dependency "
              "does not make signal part of the timeline engine ladder.",
    "format": "Format adapter layer — VST3/AU/CLAP wrappers are sanctioned "
              "engine consumers, the direction this contract exists to permit.",
    "host": "Plugin-hosting layer, likewise above the engine and sanctioned.",
    "sequence": "Sequencer runtime; sits above playback and format and consumes "
                "both. Above the top engine rung, so a floor would only restate "
                "the union of the rows beneath it.",
    "smf": "Standard MIDI File interop. Its closure (timeline, interchange, "
           "timebase, platform, runtime) is engine-shaped and a row would bind, "
           "but minting one decides where SMF sits in the ladder — a design "
           "call this gate should enforce, not make.",
    "dawproject": "DAWproject interchange. Reaches pulp::audio and so inherits "
                  "the whole audio subtree; a row derived from that would record "
                  "a wide floor as a contract. Same call as smf, deferred for "
                  "the same reason.",
}
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]pulp/([^/]+)/", re.MULTILINE)
LINK_RE = re.compile(r"\bpulp(?:::|-)([a-zA-Z0-9_-]+)\b")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".mm"}
LINK_SCOPE_KEYWORDS = {"PUBLIC", "PRIVATE", "INTERFACE"}
COMMAND_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
ARGUMENT_RE = re.compile(r'"(?:\\.|[^"\\])*"|[^\s]+')


def strip_cmake_comments(text: str) -> str:
    """Drop CMake line and block comments, preserving quoted '#' characters."""
    output: list[str] = []
    index = 0
    quoted = False
    escaped = False
    block_start = re.compile(r"#\[(=*)\[")
    while index < len(text):
        character = text[index]
        if escaped:
            output.append(character)
            escaped = False
            index += 1
            continue
        if character == "\\" and quoted:
            output.append(character)
            escaped = True
            index += 1
            continue
        if character == '"':
            output.append(character)
            quoted = not quoted
            index += 1
            continue
        if character != "#" or quoted:
            output.append(character)
            index += 1
            continue
        block = block_start.match(text, index)
        if block:
            terminator = "]" + block.group(1) + "]"
            end = text.find(terminator, block.end())
            comment_end = len(text) if end < 0 else end + len(terminator)
            output.extend("\n" for symbol in text[index:comment_end] if symbol == "\n")
            index = comment_end
            continue
        newline = text.find("\n", index)
        if newline < 0:
            break
        output.append("\n")
        index = newline + 1
    return "".join(output)


def iter_commands(cmake_text: str) -> list[tuple[str, list[str]]]:
    """Return (lowercased command name, argument tokens) for each CMake command.

    Argument tokens keep their source spelling minus surrounding quotes. The
    scan is flat: commands nested inside if()/endif() are yielded like any
    other, so a conditional link counts the same as an unconditional one. That
    over-approximation is deliberate — a floor that held only on one platform
    would not be a floor.
    """
    text = strip_cmake_comments(cmake_text)
    commands: list[tuple[str, list[str]]] = []
    cursor = 0
    while match := COMMAND_RE.search(text, cursor):
        depth = 1
        index = match.end()
        quoted = False
        escaped = False
        while index < len(text) and depth:
            character = text[index]
            if escaped:
                escaped = False
            elif character == "\\" and quoted:
                escaped = True
            elif character == '"':
                quoted = not quoted
            elif not quoted:
                if character == "(":
                    depth += 1
                elif character == ")":
                    depth -= 1
            index += 1
        arguments = [token.strip('"')
                     for token in ARGUMENT_RE.findall(text[match.end():index - 1])]
        commands.append((match.group(1).lower(), arguments))
        cursor = index
    return commands


def link_tokens(arguments: list[str]) -> list[str]:
    """Return the pulp module tokens among a target_link_libraries argument list.

    The first argument names the target being configured rather than a
    dependency, so a helper target whose own name shares the module prefix (for
    example pulp-timeline-schema-emit) is not misread as an outside-floor link.
    """
    tokens: list[str] = []
    for token in arguments[1:]:
        if token in LINK_SCOPE_KEYWORDS:
            continue
        found = LINK_RE.search(token)
        if found:
            tokens.append(found.group(1))
    return tokens


def link_dependencies(cmake_text: str) -> list[str]:
    """Return module tokens linked as dependencies via target_link_libraries.

    Every target configured in the file counts, including helper executables, so
    a build file cannot name a library outside its module's floor under any
    target name.
    """
    dependencies: list[str] = []
    for command, arguments in iter_commands(cmake_text):
        if command == "target_link_libraries":
            dependencies.extend(link_tokens(arguments))
    return dependencies


def module_key(token: str) -> str:
    """Map a link target spelling to the core/ directory that implements it."""
    if token == "view-core":
        return "view"
    return token.replace("-", "_")


def linked_module_keys(cmake_text: str) -> list[str]:
    """Return module keys reachable by linking a *library* this build file defines.

    Executables are excluded: nothing can link an executable, so a tool target
    can never enter a consumer's closure. Their dependencies are still policed,
    at depth zero, by the direct scan in verify() — this narrower view only
    decides what propagates outward to other modules.
    """
    commands = iter_commands(cmake_text)
    executables = {
        arguments[0]
        for command, arguments in commands
        if command == "add_executable" and arguments
    }
    keys: list[str] = []
    for command, arguments in commands:
        if command != "target_link_libraries" or not arguments:
            continue
        if arguments[0] in executables:
            continue
        keys.extend(module_key(token) for token in link_tokens(arguments))
    return keys


def transitive_modules(repo_root: Path, module: str) -> dict[str, tuple[str, ...]]:
    """Return every core module reachable from `module` by library links.

    Maps each reachable module key to the shortest link path that reaches it, so
    an error can name the chain rather than only the destination. Walks to a
    fixed point, so a dependency cycle terminates instead of hanging.

    A token that resolves to no core/ directory is not followed and not
    reported: targets such as pulp-tracing and pulp-cpp-httplib are build
    plumbing, not modules, and a module's own build file naming one is already
    caught at depth zero by the direct scan.
    """
    paths: dict[str, tuple[str, ...]] = {module: (module,)}
    queue = [module]
    while queue:
        current = queue.pop(0)
        cmake = repo_root / "core" / current / "CMakeLists.txt"
        if not cmake.is_file():
            continue
        for dependency in linked_module_keys(cmake.read_text(errors="replace")):
            if dependency in paths:
                continue
            if not (repo_root / "core" / dependency).is_dir():
                continue
            paths[dependency] = paths[current] + (dependency,)
            queue.append(dependency)
    return paths


def verify(repo_root: Path) -> list[str]:
    errors: list[str] = []
    for module, allowed in MODULE_FLOORS.items():
        module_root = repo_root / "core" / module
        if not module_root.is_dir():
            errors.append(f"missing required engine module: {module_root}")
            continue

        for path in sorted(module_root.rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            for match in INCLUDE_RE.finditer(path.read_text(errors="replace")):
                if match.group(1) not in allowed:
                    errors.append(
                        f"{path.relative_to(repo_root)}: {module} outside-floor "
                        f"pulp/{match.group(1)} include"
                    )

        cmake = module_root / "CMakeLists.txt"
        if not cmake.is_file():
            errors.append(f"missing {module} build file: {cmake}")
            continue
        allowed_keys = {module_key(name) for name in allowed}
        for dependency in link_dependencies(cmake.read_text(errors="replace")):
            if module_key(dependency) not in allowed_keys:
                errors.append(
                    f"{cmake.relative_to(repo_root)}: {module} outside-floor "
                    f"pulp::{dependency} link"
                )

        # A declared floor is a claim about the artifact, not about one build
        # file. Follow what the linked libraries themselves link, so a row
        # cannot stay green by depending on a module that breaches it.
        reachable = allowed_keys | LINK_CLOSURE_DEBT.get(module, set())
        for dependency, path in sorted(transitive_modules(repo_root, module).items()):
            if dependency in reachable or len(path) < 3:
                continue  # depth-one links are reported by the direct scan above
            errors.append(
                f"{cmake.relative_to(repo_root)}: {module} outside-floor "
                f"pulp::{dependency} link via " + " -> ".join(path)
            )

    errors.extend(verify_engine_consumers(repo_root))
    return errors


def verify_engine_consumers(repo_root: Path) -> list[str]:
    """Require every core/ module that links the engine to be a declared rung or an
    acknowledged consumer.

    The inverse rule — flagging engine modules that link an undeclared module —
    fires on platform, runtime, audio and midi, which are floor primitives by
    design. What is genuinely unpoliced is the other direction: a module that
    reaches *into* the engine while owning no floor of its own.
    """
    errors: list[str] = []
    declared = set(MODULE_FLOORS)
    core_root = repo_root / "core"
    if not core_root.is_dir():
        return [f"missing core directory: {core_root}"]

    consumers: dict[str, list[str]] = {}
    for entry in sorted(core_root.iterdir()):
        cmake = entry / "CMakeLists.txt"
        if not entry.is_dir() or not cmake.is_file() or entry.name in declared:
            continue
        reached = sorted(
            {module_key(token)
             for token in link_dependencies(cmake.read_text(errors="replace"))}
            & declared
        )
        if not reached:
            continue
        consumers[entry.name] = reached
        named = ", ".join(f"pulp::{name}" for name in reached)
        if entry.name not in ENGINE_CONSUMERS:
            errors.append(
                f"core/{entry.name}: links engine module(s) {named} with no "
                f"MODULE_FLOORS row and no ENGINE_CONSUMERS entry"
            )
        elif not ENGINE_CONSUMERS[entry.name].strip():
            errors.append(
                f"core/{entry.name}: ENGINE_CONSUMERS entry needs a reason"
            )

    for name in sorted(ENGINE_CONSUMERS):
        if name in declared:
            errors.append(
                f"core/{name}: ENGINE_CONSUMERS entry duplicates a MODULE_FLOORS row"
            )
        elif name not in consumers:
            errors.append(
                f"core/{name}: stale ENGINE_CONSUMERS entry — links no declared "
                f"engine module"
            )
    return errors


def run_selftest() -> int:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        fixtures: dict[str, tuple[Path, Path]] = {}
        for module, allowed in MODULE_FLOORS.items():
            module_root = root / "core" / module
            (module_root / "src").mkdir(parents=True)
            source = module_root / "src" / f"{module}.cpp"
            cmake = module_root / "CMakeLists.txt"
            source.write_text(
                "".join(
                    f"#include <pulp/{name}/allowed.hpp>\n"
                    for name in sorted(allowed)
                )
            )
            cmake.write_text(
                f"target_link_libraries(pulp-{module} PUBLIC "
                + " ".join(f"pulp::{name}" for name in sorted(allowed))
                + ")\n"
            )
            fixtures[module] = (source, cmake)

        # ENGINE_CONSUMERS names real core/ modules, so the fixture root grows a
        # stub for each. Building them from the list rather than hardcoding
        # names is what lets the stale-entry assertions below measure the list
        # itself instead of the fixture.
        consumer_cmakes: dict[str, Path] = {}
        for name in ENGINE_CONSUMERS:
            consumer_root = root / "core" / name
            consumer_root.mkdir(parents=True, exist_ok=True)
            consumer_cmake = consumer_root / "CMakeLists.txt"
            consumer_cmake.write_text(
                f"target_link_libraries(pulp-{name.replace('_', '-')} "
                "PUBLIC pulp::timeline)\n"
            )
            consumer_cmakes[name] = consumer_cmake

        if verify(root):
            print("selftest valid fixture was rejected")
            return 1

        # The concrete view target is the view module, not a separate floor.
        # Prove that spelling is accepted, then restore the forbidden production
        # chain and require the transitive walk to expose playback through it.
        timeline_view_cmake = fixtures["timeline_view"][1]
        timeline_view_links = " ".join(
            "pulp::view-core" if name == "view" else f"pulp::{name}"
            for name in sorted(MODULE_FLOORS["timeline_view"])
        )
        timeline_view_cmake.write_text(
            "target_link_libraries(pulp-timeline-view PUBLIC "
            f"{timeline_view_links})\n"
        )
        view_root = root / "core" / "view"
        view_root.mkdir(parents=True)
        view_cmake = view_root / "CMakeLists.txt"
        view_cmake.write_text("")
        if verify(root):
            print("selftest rejected pulp::view-core as the concrete view target")
            return 1

        host_cmake = consumer_cmakes["host"]
        host_cmake.write_text(
            "target_link_libraries(pulp-host PUBLIC pulp::playback)\n"
        )
        view_cmake.write_text(
            "target_link_libraries(pulp-view-core PUBLIC pulp::host)\n"
        )
        if not any(
            "timeline_view outside-floor pulp::playback link via "
            "timeline_view -> view -> host -> playback" in error
            for error in verify(root)
        ):
            print("selftest missed view-core restoring transitive playback")
            return 1
        host_cmake.write_text(
            "target_link_libraries(pulp-host PUBLIC pulp::timeline)\n"
        )
        shutil.rmtree(view_root)
        if verify(root):
            print("selftest rejected the restored playback-free view fixture")
            return 1

        # A helper target defined in a module's build file — whose own name
        # shares the module prefix and links only in-floor libraries — is not a
        # link violation, while an out-of-floor dependency of that same helper
        # still is. Guards against reading a target name as a link.
        timeline_cmake = fixtures["timeline"][1]
        floor_links = " ".join(
            f"pulp::{name}" for name in sorted(MODULE_FLOORS["timeline"]))
        timeline_cmake.write_text(
            f"target_link_libraries(pulp-timeline PUBLIC {floor_links})\n"
            "add_executable(pulp-timeline-schema-emit tools/schema_emit_main.cpp)\n"
            "target_link_libraries(pulp-timeline-schema-emit PRIVATE pulp::timeline)\n"
        )
        if verify(root):
            print("selftest rejected in-floor helper executable target")
            return 1
        timeline_cmake.write_text(
            f"target_link_libraries(pulp-timeline PUBLIC {floor_links})\n"
            "add_executable(pulp-timeline-schema-emit tools/schema_emit_main.cpp)\n"
            "target_link_libraries(pulp-timeline-schema-emit PRIVATE pulp::render)\n"
        )
        if not any(
            "timeline outside-floor pulp::render link" in error
            for error in verify(root)
        ):
            print("selftest missed helper-target outside-floor link")
            return 1
        timeline_cmake.write_text(
            f"target_link_libraries(pulp-timeline PUBLIC {floor_links})\n")

        for module, (source, cmake) in fixtures.items():
            source.write_text("#include <pulp/render/forbidden.hpp>\n")
            errors = verify(root)
            if not any(
                f"{module} outside-floor pulp/render include" in error
                for error in errors
            ):
                print(f"selftest missed {module} render include")
                return 1
            source.write_text(f"#include <pulp/{module}/allowed.hpp>\n")

            cmake.write_text(f"target_link_libraries(pulp-{module} PUBLIC pulp::render)\n")
            errors = verify(root)
            if not any(
                f"{module} outside-floor pulp::render link" in error
                for error in errors
            ):
                print(f"selftest missed {module} namespaced render link")
                return 1

            cmake.write_text(f"target_link_libraries(pulp-{module} PUBLIC pulp-render)\n")
            if not any(
                f"{module} outside-floor pulp::render link" in error
                for error in verify(root)
            ):
                print(f"selftest missed {module} raw render link")
                return 1

            allowed = MODULE_FLOORS[module]
            cmake.write_text(
                f"target_link_libraries(pulp-{module} PUBLIC "
                + " ".join(f"pulp::{name}" for name in sorted(allowed))
                + ")\n"
            )

        # The editor rung's defining rule is that it cannot reach playback, and
        # playback — unlike the render module the loop above uses — is itself a
        # declared engine module. Assert it by name in both directions so the
        # rule cannot be weakened by widening some other row.
        editor_source, editor_cmake = fixtures["timeline_editor"]
        editor_source.write_text("#include <pulp/playback/transport.hpp>\n")
        if not any(
            "timeline_editor outside-floor pulp/playback include" in error
            for error in verify(root)
        ):
            print("selftest missed timeline_editor playback include")
            return 1
        editor_source.write_text("#include <pulp/timeline_editor/allowed.hpp>\n")

        editor_cmake.write_text(
            "target_link_libraries(pulp-timeline-editor INTERFACE "
            "pulp::timeline pulp::timebase pulp::playback)\n"
        )
        if not any(
            "timeline_editor outside-floor pulp::playback link" in error
            for error in verify(root)
        ):
            print("selftest missed timeline_editor playback link")
            return 1
        editor_cmake.write_text(
            "target_link_libraries(pulp-timeline-editor INTERFACE "
            "pulp::timeline pulp::timebase)\n"
        )
        if verify(root):
            print("selftest rejected the restored timeline_editor fixture")
            return 1

        # The rung's other defining rule, and the one that decides where the edit
        # vocabulary lives: the document model may not reach up for it. Both rows
        # forbid view, so only this direction distinguishes the two addresses.
        timeline_source = fixtures["timeline"][0]
        timeline_source.write_text("#include <pulp/timeline_editor/edit_intent.hpp>\n")
        if not any(
            "timeline outside-floor pulp/timeline_editor include" in error
            for error in verify(root)
        ):
            print("selftest missed timeline reaching up for the editor rung")
            return 1
        timeline_source.write_text("#include <pulp/timeline/allowed.hpp>\n")

        timeline_cmake.write_text(
            f"target_link_libraries(pulp-timeline PUBLIC {floor_links} "
            "pulp::timeline-editor)\n"
        )
        if not any(
            "timeline outside-floor pulp::timeline-editor link" in error
            for error in verify(root)
        ):
            print("selftest missed timeline linking the editor rung")
            return 1
        timeline_cmake.write_text(
            f"target_link_libraries(pulp-timeline PUBLIC {floor_links})\n")
        if verify(root):
            print("selftest rejected the restored timeline fixture")
            return 1

        # A row that is honest in its own build file and breached through a
        # dependency. `playback` links `pulp::audio` legitimately; `audio` owns
        # no row, and it is what reaches `render`. Only a transitive walk can
        # see this, and it is the shape the whole check exists to catch.
        (root / "core" / "render").mkdir(parents=True)
        (root / "core" / "render" / "CMakeLists.txt").write_text("")
        audio_root = root / "core" / "audio"
        audio_root.mkdir(parents=True)
        audio_cmake = audio_root / "CMakeLists.txt"
        audio_cmake.write_text("target_link_libraries(pulp-audio PUBLIC pulp::render)\n")
        if not any(
            "playback outside-floor pulp::render link via playback -> audio -> render"
            in error
            for error in verify(root)
        ):
            print("selftest missed playback reaching render through audio")
            return 1

        # An executable cannot be linked, so a tool target in a dependency must
        # not raise that dependency's consumers. This is the false-positive
        # guard: without it the closure would charge playback for every helper
        # binary anywhere beneath it.
        audio_cmake.write_text(
            "add_executable(pulp-audio-dump tools/dump_main.cpp)\n"
            "target_link_libraries(pulp-audio-dump PRIVATE pulp::render)\n"
        )
        if verify(root):
            print("selftest charged playback for a dependency's executable target")
            return 1

        # A dependency cycle must reach a fixed point rather than recurse: the
        # assertion is that this returns at all. The cycle also points `audio`
        # back at a declared module, so the consumer rule fires on it — asserted
        # here too, since a rule that only ever fires on a purpose-built fixture
        # has not been shown to fire on an ordinary one.
        audio_cmake.write_text("target_link_libraries(pulp-audio PUBLIC pulp::playback)\n")
        cycle_errors = verify(root)
        if [error for error in cycle_errors if "outside-floor" in error]:
            print("selftest rejected a benign playback/audio link cycle")
            return 1
        if not any(
            "core/audio: links engine module(s) pulp::playback" in error
            for error in cycle_errors
        ):
            print("selftest missed audio reaching playback with no row")
            return 1

        # LINK_CLOSURE_DEBT admits exactly what it names. `state` is recorded
        # there for playback; `render` above is not, and neither is `view`.
        (root / "core" / "state").mkdir(parents=True)
        (root / "core" / "state" / "CMakeLists.txt").write_text("")
        audio_cmake.write_text("target_link_libraries(pulp-audio PUBLIC pulp::state)\n")
        if verify(root):
            print("selftest rejected a link closure entry declared in LINK_CLOSURE_DEBT")
            return 1
        (root / "core" / "view").mkdir(parents=True)
        (root / "core" / "view" / "CMakeLists.txt").write_text("")
        audio_cmake.write_text("target_link_libraries(pulp-audio PUBLIC pulp::view)\n")
        if not any(
            "playback outside-floor pulp::view link via playback -> audio -> view" in error
            for error in verify(root)
        ):
            print("selftest let an undeclared module through the link closure")
            return 1
        shutil.rmtree(audio_root)
        if verify(root):
            print("selftest rejected the restored fixture after the closure checks")
            return 1

        # A core/ module that reaches into the engine while owning no floor.
        #
        # The name must be one no real module will ever take: the assertion
        # needs this consumer to be *undeclared*, so naming it after a module
        # that does not exist yet is correct only until someone creates one.
        # That already happened once — the fixture was `timeline_view`, which
        # became a real rung, and the mkdir then collided with a directory the
        # fixture tree already carried. The guard below turns that from a
        # confusing FileExistsError into a statement of what broke.
        consumer_name = "selftest_undeclared_consumer"
        if consumer_name in MODULE_FLOORS or consumer_name in ENGINE_CONSUMERS:
            print(
                f"selftest fixture name {consumer_name!r} is now a declared "
                "module; the undeclared-consumer assertion cannot hold. "
                "Rename the fixture."
            )
            return 1
        consumer_root = root / "core" / consumer_name
        if consumer_root.exists():
            print(
                f"selftest fixture name {consumer_name!r} already exists under "
                "core/; the undeclared-consumer assertion cannot hold. "
                "Rename the fixture."
            )
            return 1
        consumer_root.mkdir(parents=True)
        consumer_cmake = consumer_root / "CMakeLists.txt"
        consumer_cmake.write_text(
            "target_link_libraries(pulp-selftest-undeclared-consumer PUBLIC "
            "pulp::view pulp::playback)\n"
        )
        if not any(
            f"core/{consumer_name}: links engine module(s) pulp::playback" in error
            for error in verify(root)
        ):
            print("selftest missed an undeclared engine consumer")
            return 1
        shutil.rmtree(consumer_root)
        if verify(root):
            print("selftest rejected the restored fixture after the consumer check")
            return 1

        # The allowlist is a decision record, so an entry with no reason and an
        # entry whose subject no longer reaches the engine are both rejected.
        # Without these the list would be free to grow and to rot.
        sample = sorted(ENGINE_CONSUMERS)[0]
        ENGINE_CONSUMERS[sample] = "   "
        if not any(
            f"core/{sample}: ENGINE_CONSUMERS entry needs a reason" in error
            for error in verify(root)
        ):
            print("selftest accepted an ENGINE_CONSUMERS entry with no reason")
            return 1
        ENGINE_CONSUMERS[sample] = "restored for the remaining assertions"

        consumer_cmakes[sample].write_text(
            f"target_link_libraries(pulp-{sample.replace('_', '-')} PUBLIC pulp::runtime)\n"
        )
        if not any(
            f"core/{sample}: stale ENGINE_CONSUMERS entry" in error
            for error in verify(root)
        ):
            print("selftest accepted a stale ENGINE_CONSUMERS entry")
            return 1
        consumer_cmakes[sample].write_text(
            f"target_link_libraries(pulp-{sample.replace('_', '-')} PUBLIC pulp::timeline)\n"
        )

        ENGINE_CONSUMERS["timeline"] = "a row and an allowlist entry are exclusive"
        if not any(
            "core/timeline: ENGINE_CONSUMERS entry duplicates a MODULE_FLOORS row" in error
            for error in verify(root)
        ):
            print("selftest accepted an ENGINE_CONSUMERS entry that duplicates a row")
            return 1
        del ENGINE_CONSUMERS["timeline"]
        if verify(root):
            print("selftest rejected the restored fixture after the allowlist checks")
            return 1

        shutil.rmtree(root / "core" / "timeline")
        if not any("missing required engine module" in error for error in verify(root)):
            print("selftest missed absent required timeline module")
            return 1

    print("timeline_engine_dependency_floor_selftest=true")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return run_selftest()

    errors = verify(args.repo_root.resolve())
    if errors:
        for error in errors:
            print(error)
        return 1
    print("timeline_engine_dependency_floor_verified=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
