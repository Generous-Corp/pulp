#!/usr/bin/env python3
"""One answer to "where are the tools", used by everything that asks.

There were two implementations of this, and they drifted. The module builder
kept its own, which returned the bare string "claude" when it found nothing --
so launched from a host, where there is no PATH to search, it reached the user
as a raw `FileNotFoundError: 'claude'` traceback inside the plugin's own
window, under a heading saying ATTENTION.

    python3 tools/rack/test_toolpaths.py
"""
from __future__ import annotations

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import toolpaths                                       # noqa: E402


def main() -> int:
    bad = 0

    # Nobody may keep a private answer. A second lookup is how this broke.
    for name in ("generate.py", "patch.py"):
        src = open(os.path.join(HERE, name)).read()
        if "toolpaths" not in src:
            print(f"  WRONG  {name} does not ask toolpaths where the tools are")
            bad += 1
        else:
            print(f"  ok     {name} asks toolpaths")

    # Resolved with NO environment, which is what a host-launched plugin has.
    for name in ("generate", "patch"):
        r = subprocess.run(
            [sys.executable, "-c",
             f"import sys; sys.path.insert(0,'.'); import {name}; "
             f"print({name}.find_claude())"],
            cwd=HERE, capture_output=True, text=True,
            env={"HOME": os.path.expanduser("~")})
        got = r.stdout.strip()
        if r.returncode != 0:
            # Refusing with a reason is a fine answer; it names where it looked.
            if "could not find" in (r.stdout + r.stderr):
                print(f"  ok     {name}: refuses, and says where it looked")
                continue
            print(f"  WRONG  {name}: failed without saying why: "
                  f"{(r.stdout + r.stderr).strip()[:160]}")
            bad += 1
            continue
        if got == "claude":
            print(f"  WRONG  {name}: returned the bare string 'claude' with no "
                  f"PATH to run it from — that reaches a user as a traceback")
            bad += 1
        elif not os.path.isabs(got):
            print(f"  WRONG  {name}: returned {got!r}, which is not a real path")
            bad += 1
        else:
            print(f"  ok     {name}: resolves an absolute path with no PATH set")

    # Every module-level constant a generator REFERS to must exist.
    #
    # Editing one of them removed its constants along with an old helper, and
    # nothing noticed until a plugin inside REAPER printed
    # `NameError: name 'SDK' is not defined` into its own window. Importing a
    # function from the file does not catch that, and neither does running it
    # with no arguments -- usage prints and exits long before the line that
    # would have failed. So the names are checked statically: every ALL-CAPS
    # identifier the source uses has to be something the imported module
    # actually has.
    import ast as _ast
    import importlib
    for name in ("generate", "patch"):
        src = open(os.path.join(HERE, name + ".py")).read()
        tree = _ast.parse(src)
        # EVERY name used and never bound, not just the shouty ones. Limiting
        # this to ALL-CAPS missed `_assert_no_duplicate_models`, which was
        # called on the last step of every successful module build and never
        # defined anywhere -- a NameError waiting in the original.
        used = {n.id for n in _ast.walk(tree)
                if isinstance(n, _ast.Name) and isinstance(n.ctx, _ast.Load)}
        # Anything the file BINDS anywhere is fine: assignments, function
        # arguments, imports (including inside a function), comprehension
        # targets, except-clauses, defs and classes. The question is only
        # whether a name is used and never bound at all.
        bound: set[str] = set()
        for n in _ast.walk(tree):
            if isinstance(n, _ast.Name) and isinstance(n.ctx, _ast.Store):
                bound.add(n.id)
            elif isinstance(n, _ast.arg):
                bound.add(n.arg)
            elif isinstance(n, _ast.alias):
                bound.add((n.asname or n.name).split(".")[0])
            elif isinstance(n, (_ast.FunctionDef, _ast.AsyncFunctionDef,
                                _ast.ClassDef)):
                bound.add(n.name)
            elif isinstance(n, _ast.ExceptHandler) and n.name:
                bound.add(n.name)
            elif isinstance(n, (_ast.Global, _ast.Nonlocal)):
                bound.update(n.names)
        used -= bound
        mod = importlib.import_module(name)
        import builtins as _b
        # Names bound by imports, comprehensions, arguments and so on are all
        # attributes of the module or builtins by the time it is imported.
        missing = sorted(u for u in used
                         if not hasattr(mod, u) and not hasattr(_b, u))
        if missing:
            print(f"  WRONG  {name}.py refers to {missing}, which it does not "
                  f"define — that reaches a user as a NameError traceback")
            bad += 1
        else:
            print(f"  ok     {name}.py defines every constant it refers to")

    # The environment handed to the tool has to be able to find node, because
    # claude runs its own hooks with it.
    env = toolpaths.tool_env()
    if "/opt/homebrew/bin" not in env["PATH"] and os.path.isdir("/opt/homebrew/bin"):
        print("  WRONG  the tool environment cannot find Homebrew, so a hook "
              "that shells out to node dies naming node")
        bad += 1
    else:
        print("  ok     the tool environment can find what the tool shells out to")

    print("\nFAILED" if bad else "\nall good")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
