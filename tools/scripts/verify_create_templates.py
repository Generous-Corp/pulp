#!/usr/bin/env python3
"""Template integrity smoke for `pulp create`.

`pulp create <name> --type <effect|instrument|app|bare>` materializes a
buildable Pulp project by walking `tools/templates/<type>/*.template` and
substituting `{{VAR}}` placeholders. The Chainer cross-lane plan
(`planning/2026-05-24-linux-macos-chainer-gap-closure-plan.md`, Phase 1
"Materialize Chainer Project") depends on the materializer being
deterministic and bitrot-free.

Today nothing guards the template tree itself: a template that picks up
a stray `{{NEW_VAR}}` not in the substitution map in
`tools/cli/cmd_create.cpp` will silently ship to users with the
unsubstituted literal in their generated source. Likewise, deleting a
required template file (e.g. `instrument/CMakeLists.txt.template`) only
surfaces the next time someone runs `pulp create --type instrument`.

This script is the structural guard:

  1. Every `{{VAR}}` referenced by a `.template` file is listed in
     `KNOWN_TEMPLATE_VARS`.
  2. Every plugin/app template directory ships the minimum required
     files for that template type.

It does NOT build anything. It does NOT need the CLI binary. It runs
in seconds on every supported platform (Linux x86_64, Linux arm64,
macOS, Windows), which makes it the right shape for a public-CI gate
on a host (e.g. the user's Ubuntu arm64 SSH host) that cannot ship the
GPU/Skia-gated CLI build.

Exit codes:
    0 — all templates passed structural validation.
    1 — one or more templates failed (details printed to stderr).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Keep this list in sync with the `vars` substitution map in
# `tools/cli/cmd_create.cpp` (the inline `std::vector<std::pair<...>>`
# inside `cmd_create()`). If you add a new substitution there, add it
# here too — this script's whole point is to fail loudly when those two
# lists drift apart.
KNOWN_TEMPLATE_VARS = frozenset({
    "PLUGIN_NAME",
    "CLASS_NAME",
    "LOWER_NAME",
    "PLUGIN_URI",
    "NAMESPACE",
    "FACTORY_NAME",
    "HEADER_NAME",
    "TARGET_NAME",
    "MANUFACTURER",
    "MANUFACTURER_CODE",
    "BUNDLE_ID",
    "VERSION",
    "PLUGIN_CODE",
    "AAX_PRODUCT_CODE",
    "AAX_NATIVE_CODE",
    "FORMATS",
    "DESCRIPTION",
    "VST3_UID",
    "SDK_VERSION",
})

# Per-template-type minimum file set. Missing any of these fails — the
# template is broken in a way that `pulp create --type <type>` would
# silently materialize an unbuildable project.
REQUIRED_FILES_BY_TYPE = {
    "effect": {
        "CMakeLists.txt.template",
        "processor.hpp.template",
        "test.cpp.template",
        "au_v2_entry.cpp.template",
        "clap_entry.cpp.template",
        "vst3_entry.cpp.template",
    },
    "instrument": {
        "CMakeLists.txt.template",
        "processor.hpp.template",
        "test.cpp.template",
        "au_v2_entry.cpp.template",
        "clap_entry.cpp.template",
        "vst3_entry.cpp.template",
    },
    "app": {
        "CMakeLists.txt.template",
        "processor.hpp.template",
        "test.cpp.template",
    },
    "bare": {
        "CMakeLists.txt.template",
        "processor.hpp.template",
        "test.cpp.template",
    },
    # `gain` is a named template (--template gain) that uses the
    # effect surface.
    "gain": {
        "CMakeLists.txt.template",
        "processor.hpp.template",
        "test.cpp.template",
    },
}

_VAR_PATTERN = re.compile(r"\{\{([A-Z_][A-Z0-9_]*)\}\}")


def scan_template_vars(text: str) -> set[str]:
    """Return the set of `{{VAR}}` names referenced in `text`."""
    return set(_VAR_PATTERN.findall(text))


def check_template_dir(type_name: str, dir_path: Path) -> list[str]:
    """Return a list of human-readable failure messages for `dir_path`.

    Recurses into nested directories under `dir_path` so trees like
    ``tools/templates/android/app/src/main/...`` and
    ``tools/templates/standalone/<type>/CMakeLists.txt.template`` are
    covered too — `pulp create` materializes those via
    ``recursive_directory_iterator``, so any `{{VAR}}` placeholder in
    them must be in the substitution map or the generated project
    silently ships the literal text.

    Regression: Codex PR #3002 review (P1 + P2). The previous
    implementation called `dir_path.glob("*.template")` which only
    matched immediate children, so nested template trees escaped the
    smoke entirely.
    """
    failures: list[str] = []
    required = REQUIRED_FILES_BY_TYPE.get(type_name)
    if required is None:
        # Template type isn't in the required-set map (e.g. android,
        # auv3, standalone, from-figma, from-v0). Those are partial
        # template trees that compose with another type; their
        # structural rules differ and are out of scope for this smoke.
        # We still validate every `{{VAR}}` they reference.
        pass
    else:
        present = {p.name for p in dir_path.glob("*.template")}
        missing = required - present
        if missing:
            failures.append(
                f"[{type_name}] missing required template files: "
                + ", ".join(sorted(missing))
            )

    for template_file in sorted(dir_path.rglob("*.template")):
        try:
            text = template_file.read_text(encoding="utf-8")
        except UnicodeDecodeError as e:
            failures.append(
                f"[{type_name}] {template_file.relative_to(dir_path)}: "
                f"not valid UTF-8: {e}"
            )
            continue
        unknown = scan_template_vars(text) - KNOWN_TEMPLATE_VARS
        if unknown:
            failures.append(
                f"[{type_name}] {template_file.relative_to(dir_path)}: "
                f"references unknown template variable(s): "
                + ", ".join(f"{{{{{v}}}}}" for v in sorted(unknown))
                + " (add to KNOWN_TEMPLATE_VARS here and to the "
                  "substitution map in tools/cli/cmd_create.cpp)"
            )
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Structural smoke for pulp create templates",
    )
    parser.add_argument(
        "--templates-root",
        type=Path,
        default=None,
        help=(
            "Path to tools/templates/ (default: resolved relative to "
            "this script)"
        ),
    )
    args = parser.parse_args(argv)

    if args.templates_root is None:
        # tools/scripts/verify_create_templates.py  →  tools/templates/
        script_dir = Path(__file__).resolve().parent
        templates_root = script_dir.parent / "templates"
    else:
        templates_root = args.templates_root

    if not templates_root.is_dir():
        print(
            f"verify_create_templates: templates root not found: "
            f"{templates_root}",
            file=sys.stderr,
        )
        return 1

    all_failures: list[str] = []
    # Use rglob so trees like `standalone/<type>/CMakeLists.txt.template`
    # are recognised even though their immediate parent has no top-level
    # .template files. Regression: Codex PR #3002 P1.
    type_dirs = [
        d for d in sorted(templates_root.iterdir())
        if d.is_dir() and any(d.rglob("*.template"))
    ]
    if not type_dirs:
        print(
            f"verify_create_templates: no template directories with "
            f"*.template files under {templates_root}",
            file=sys.stderr,
        )
        return 1

    for type_dir in type_dirs:
        all_failures.extend(check_template_dir(type_dir.name, type_dir))

    if all_failures:
        for line in all_failures:
            print(line, file=sys.stderr)
        print(
            f"\nverify_create_templates: {len(all_failures)} failure(s) "
            f"across {len(type_dirs)} template directory/ies",
            file=sys.stderr,
        )
        return 1

    print(
        f"verify_create_templates: {len(type_dirs)} template directories "
        f"OK — all `{{{{VAR}}}}` references match the substitution map"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
