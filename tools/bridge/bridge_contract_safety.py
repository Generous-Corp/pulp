#!/usr/bin/env python3
"""Audit generated bridge identifiers before a contract reaches C++ or TypeScript.

The bridge generator validates TOML identifiers, but those identifiers are later
transformed into C++ struct names and TypeScript wrapper names.  Different source
names can therefore collide after transformation, and a valid TOML identifier
can still be a reserved word in one generated language.  This small audit keeps
those failures close to the contract source without changing the generator's
stable output format.
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
from pathlib import Path
from typing import Any


HERE = Path(__file__).resolve().parent
GENERATOR_PATH = HERE / "bridge_gen.py"
spec = importlib.util.spec_from_file_location("pulp_bridge_generator", GENERATOR_PATH)
if spec is None or spec.loader is None:
    raise RuntimeError(f"could not load bridge generator: {GENERATOR_PATH}")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


CPP_KEYWORDS = frozenset(
    """
    alignas alignof and and_eq asm atomic_cancel atomic_commit atomic_noexcept auto
    bitand bitor bool break case catch char char8_t char16_t char32_t class compl
    concept const consteval constexpr constinit const_cast continue co_await co_return
    co_yield decltype default delete do double dynamic_cast else enum explicit export
    extern false float for friend goto if inline int long mutable namespace new noexcept
    not not_eq nullptr operator or or_eq private protected public reflexpr register
    reinterpret_cast requires return short signed sizeof static static_assert static_cast
    struct switch synchronized template this thread_local throw true try typedef typeid
    typename union unsigned using virtual void volatile wchar_t while xor xor_eq
    """.split()
)

TS_KEYWORDS = frozenset(
    """
    abstract await break case catch class const continue debugger default delete do
    else enum export extends false finally for function if implements import in
    instanceof interface let new null package private protected public return static
    super switch this throw true try typeof var void while with yield
    """.split()
)

# Contextual TypeScript words (for example ``type``, ``get``, ``from`` and
# ``is``) remain legal binding identifiers.  Only strict/future-reserved words
# plus the two strict-mode restricted bindings are rejected here.
TS_PARAMETER_KEYWORDS = TS_KEYWORDS | frozenset({"arguments", "eval"})
TS_WRAPPER_RESERVED = TS_PARAMETER_KEYWORDS

TS_GENERATED_ALIASES = {
    "EditorBridgeCommand",
    "EditorBridgePublication",
    "EditorBridgeRequest",
    "EditorBridgeResponse",
    "EditorBridgeResult",
    "EditorBridgeTransport",
}


def _cpp_reserved(name: str) -> bool:
    return (
        name in CPP_KEYWORDS
        or name.startswith("__")
        or (name.startswith("_") and len(name) > 1 and name[1].isupper())
    )


def _all_rows(data: dict[str, Any]) -> list[tuple[str, dict[str, Any], str]]:
    rows: list[tuple[str, dict[str, Any], str]] = []
    rows.extend(("commands", row, "request") for row in data["commands"])
    rows.extend(("commands", row, "response") for row in data["commands"])
    rows.extend(("publications", row, "fields") for row in data["publications"])
    return rows


def audit(data: dict[str, Any]) -> list[str]:
    """Return deterministic source locations for generated identifier hazards."""

    problems: list[str] = []
    symbols: dict[str, str] = {}
    wrapper_names: dict[str, str] = {}

    def claim(symbol: str, source: str) -> None:
        if symbol in TS_GENERATED_ALIASES:
            problems.append(
                f"generated TypeScript interface collides with alias: {symbol} ({source})"
            )
        previous = symbols.get(symbol)
        if previous is None:
            symbols[symbol] = source
        elif previous != source:
            problems.append(f"generated C++ type collision: {symbol} ({previous}, {source})")

    for section, row, field_key in _all_rows(data):
        for field in row.get(field_key, []):
            name = field["name"]
            if _cpp_reserved(name):
                problems.append(
                    f"{section}.{row['name']}.{field_key} field "
                    f"'{name}' is reserved in C++"
                )
            if section == "commands" and field_key == "request":
                if name in TS_PARAMETER_KEYWORDS:
                    problems.append(
                        f"{section}.{row['name']}.{field_key} field "
                        f"'{name}' is reserved in TypeScript parameter position"
                    )
                if name == "transport":
                    problems.append(
                        f"{section}.{row['name']}.{field_key} field "
                        "'transport' collides with the generated TypeScript transport parameter"
                    )

    for row in data["commands"]:
        title = generator.type_name(row["name"])
        if not title:
            problems.append(f"commands.{row['name']} produces an empty generated type name")
        claim(f"{title}Request", f"commands.{row['name']}.request")
        claim(f"{title}Response", f"commands.{row['name']}.response")
        wrapper = generator.camel_name(row["name"])
        if not wrapper:
            problems.append(f"commands.{row['name']} produces an empty TypeScript wrapper name")
        elif wrapper in TS_WRAPPER_RESERVED:
            problems.append(
                f"commands.{row['name']} wrapper '{wrapper}' is reserved in TypeScript"
            )
        previous = wrapper_names.get(wrapper)
        if previous is None:
            wrapper_names[wrapper] = row["name"]
        elif previous != row["name"]:
            problems.append(
                f"generated TypeScript wrapper collision: {wrapper} "
                f"({previous}, {row['name']})"
            )

    for row in data["publications"]:
        if not generator.type_name(row["name"]):
            problems.append(
                f"publications.{row['name']} produces an empty generated type name"
            )
        claim(
            f"{generator.type_name(row['name'])}Frame",
            f"publications.{row['name']}.fields",
        )

    return sorted(set(problems))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=generator.SOURCE)
    args = parser.parse_args(argv)
    try:
        problems = audit(generator.load_contract(args.source))
    except ValueError as exc:
        parser.error(str(exc))
    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        return 1
    print(f"bridge contract safety: OK ({args.source})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
