#!/usr/bin/env python3
"""Reject inspector wire methods that bypass protocol_methods.inc."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import tempfile


REGISTRY_ENTRY = re.compile(
    r'PULP_INSPECT_METHOD\([^,]+,\s*"([A-Z][A-Za-z0-9]*\.[a-z][A-Za-z0-9]*)"'
)
WIRE_NAME = re.compile(r"(?<![A-Za-z0-9])([A-Z][A-Za-z0-9]*\.[a-z][A-Za-z0-9]*)")
WIRE_LITERAL = re.compile(r'"([A-Z][A-Za-z0-9]*\.[a-z][A-Za-z0-9]*)"')
RAW_LITERAL = re.compile(
    r'R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\((?P<body>.*?)\)(?P=delimiter)"',
    re.DOTALL,
)
ADJACENT_LITERALS = re.compile(r'(?:"(?:\\.|[^"\\])*"\s*){2,}', re.DOTALL)
STRING_TOKEN = re.compile(r'"((?:\\.|[^"\\])*)"')
SOURCE_SUFFIXES = {".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".mm", ".rs"}
NON_PROTOCOL_LITERALS = {
    "A.clap", "AAX.h", "AGENTS.md", "Acme.clap", "App.tsx", "Array.prototype",
    "AudioToolbox.framework", "B.vst3", "Babel.transform", "CHANGELOG.md",
    "CLAUDE.md", "CMakeCache.txt", "CMakeLists.txt",
    "CanvasRenderingContext2D.clearRect", "Color.clear", "Component.tsx",
    "Compressor.vst3", "DBusObjectPathVTable.message", "DEPENDENCIES.md",
    "DESIGN.md", "Date.now", "Foo.vst3", "GainStage.tsx", "HANDOFF.md",
    "Info.plist", "JSON.h", "JSON.stringify", "KEYS.len", "KeyboardEvent.key", "LICENSE.md",
    "Math.random", "My.clap", "MyPlugin.component", "NOTICE.md", "ObjBase.h",
    "Object.create", "Object.defineProperty", "Promise.resolve", "Promise.then",
    "PulpAccessibility.kt", "PulpConfig.cmake", "PulpDelay.lv2", "PulpGain.clap",
    "PulpReverb.component", "PulpTargets.cmake", "PulpWobble.vst3", "README.md",
    "ReactDOM.createRoot", "ReactDOM.flushSync", "SKILL.md", "Same.clap",
    "StringUtilities.h", "Three.js", "Tools.x86", "TransportBar.tsx",
    "UIAutomation.h", "WebView2Loader.dll", "WheelEvent.deltaY", "Xcode.app",
    "Xft.dpi",
}


def registered_methods(registry: pathlib.Path) -> set[str]:
    methods = set(REGISTRY_ENTRY.findall(registry.read_text(encoding="utf-8")))
    if not methods:
        raise ValueError(f"no protocol methods found in {registry}")
    return methods


def wire_literals(text: str) -> list[tuple[int, str]]:
    found = [(match.start(), match.group(1)) for match in WIRE_LITERAL.finditer(text)]
    for raw in RAW_LITERAL.finditer(text):
        for method in WIRE_NAME.finditer(raw.group("body")):
            found.append((raw.start("body") + method.start(), method.group(1)))
    for sequence in ADJACENT_LITERALS.finditer(text):
        joined = "".join(token.group(1) for token in STRING_TOKEN.finditer(sequence.group()))
        for method in WIRE_NAME.finditer(joined):
            found.append((sequence.start(), method.group(1)))
    return sorted(set(found))


def unmapped_literals(root: pathlib.Path, registry: pathlib.Path) -> list[tuple[pathlib.Path, int, str]]:
    registered = registered_methods(registry)
    generated_header = root / "inspect/include/pulp/inspect/protocol.hpp"
    failures: list[tuple[pathlib.Path, int, str]] = []
    sources = list((root / "inspect").rglob("*"))
    sources.extend((root / "core").rglob("*"))
    sources.extend((root / "experimental/pulp-rs/src").rglob("*.rs"))
    sources.extend((root / "tools/cli").rglob("*"))
    sources.extend((root / "tools/mcp").rglob("*"))
    for source in sorted(set(sources)):
        if not source.is_file():
            continue
        if source.suffix not in SOURCE_SUFFIXES or source == registry:
            continue
        # protocol.hpp materializes constants from the registry and contains
        # documentation examples; it is not an independent dispatch surface.
        if source == generated_header:
            continue
        text = source.read_text(encoding="utf-8")
        for offset, method in wire_literals(text):
            if method in NON_PROTOCOL_LITERALS:
                continue
            if method not in registered:
                failures.append(
                    (source.relative_to(root), text.count("\n", 0, offset) + 1, method)
                )
    return failures


def run_check(root: pathlib.Path) -> int:
    registry = root / "inspect/include/pulp/inspect/protocol_methods.inc"
    failures = unmapped_literals(root, registry)
    if failures:
        print("inspector protocol literals missing from protocol_methods.inc:", file=sys.stderr)
        for path, line, method in failures:
            print(f"  {path}:{line}: {method}", file=sys.stderr)
        return 1
    print(f"inspector protocol registry complete ({len(registered_methods(registry))} methods)")
    return 0


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="pulp-inspector-registry-") as temporary:
        root = pathlib.Path(temporary)
        registry = root / "inspect/include/pulp/inspect/protocol_methods.inc"
        source = root / "inspect/src/handler.cpp"
        rust_source = root / "experimental/pulp-rs/src/shared_inspector_client.rs"
        cpp_client_source = root / "tools/cli/new_inspector_client.cpp"
        generated_header = root / "inspect/include/pulp/inspect/protocol.hpp"
        same_named_source = root / "tools/mcp/protocol.hpp"
        registry.parent.mkdir(parents=True)
        source.parent.mkdir(parents=True)
        rust_source.parent.mkdir(parents=True)
        cpp_client_source.parent.mkdir(parents=True)
        same_named_source.parent.mkdir(parents=True)
        registry.write_text(
            'PULP_INSPECT_METHOD(Known, "Known.method", Observe, SessionDescribe, No)\n',
            encoding="utf-8",
        )
        source.write_text('auto method = "Known.method";\n', encoding="utf-8")
        rust_source.write_text('let method = "Known.method";\n', encoding="utf-8")
        cpp_client_source.write_text('auto method = "Known.method";\n', encoding="utf-8")
        generated_header.write_text(
            'auto example = "GeneratedHeader.example";\n', encoding="utf-8"
        )
        same_named_source.write_text('auto method = "Known.method";\n', encoding="utf-8")
        if unmapped_literals(root, registry):
            print("self-test rejected a registered method", file=sys.stderr)
            return 1
        rust_source.write_text('let method = "PhaseEight.unmapped";\n', encoding="utf-8")
        failures = unmapped_literals(root, registry)
        if len(failures) != 1 or failures[0][2] != "PhaseEight.unmapped":
            print("self-test failed to reject an unmapped Rust method", file=sys.stderr)
            return 1
        rust_source.write_text('let method = "Known.method";\n', encoding="utf-8")
        cpp_client_source.write_text(
            'auto method = "PhaseEight.unmapped";\n', encoding="utf-8"
        )
        failures = unmapped_literals(root, registry)
        if len(failures) != 1 or failures[0][2] != "PhaseEight.unmapped":
            print("self-test failed to reject an unmapped C++ client method", file=sys.stderr)
            return 1
        cpp_client_source.write_text(
            'auto method = R"(PhaseEight.unmapped)";\n', encoding="utf-8"
        )
        failures = unmapped_literals(root, registry)
        if len(failures) != 1 or failures[0][2] != "PhaseEight.unmapped":
            print("self-test failed to reject a raw C++ method", file=sys.stderr)
            return 1
        cpp_client_source.write_text(
            'auto method = "PhaseEight." "unmapped";\n', encoding="utf-8"
        )
        failures = unmapped_literals(root, registry)
        if len(failures) != 1 or failures[0][2] != "PhaseEight.unmapped":
            print("self-test failed to reject an adjacent C++ method", file=sys.stderr)
            return 1
        cpp_client_source.write_text('auto method = "Runtime.app";\n', encoding="utf-8")
        failures = unmapped_literals(root, registry)
        if len(failures) != 1 or failures[0][2] != "Runtime.app":
            print("self-test allowed a protocol-looking filename suffix", file=sys.stderr)
            return 1
        cpp_client_source.write_text('auto method = "Known.method";\n', encoding="utf-8")
        same_named_source.write_text(
            'auto method = "PhaseEight.sameNamedHeader";\n', encoding="utf-8"
        )
        failures = unmapped_literals(root, registry)
        if len(failures) != 1 or failures[0] != (
            pathlib.Path("tools/mcp/protocol.hpp"),
            1,
            "PhaseEight.sameNamedHeader",
        ):
            print("self-test exempted an unrelated protocol.hpp", file=sys.stderr)
            return 1
    print("inspector protocol registry checker self-test passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2])
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    return self_test() if args.self_test else run_check(args.root.resolve())


if __name__ == "__main__":
    raise SystemExit(main())
