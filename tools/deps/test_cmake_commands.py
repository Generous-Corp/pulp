#!/usr/bin/env python3
"""Tests for tools/deps/cmake_commands.py, the CMake command tokenizer."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "deps"))
import cmake_commands as cc  # noqa: E402

FAKE = "fetchcontent_declare(vellum_gpu_runtime GIT_TAG main)"


def names(text: str) -> list[str]:
    return [command.name for command in cc.tokenize(text)]


class TokenizeTests(unittest.TestCase):
    def test_commands_and_argument_kinds(self) -> None:
        (command,) = cc.tokenize('Set(a "b c" [[d]] e)\n')
        self.assertEqual(command.name, "set")
        self.assertEqual(command.arguments, (
            cc.Argument("a", cc.UNQUOTED), cc.Argument("b c", cc.QUOTED),
            cc.Argument("d", cc.BRACKET), cc.Argument("e", cc.UNQUOTED)))

    def test_text_in_comments_strings_and_brackets_is_never_a_command(self) -> None:
        cases = {
            "line comment": f"# {FAKE}\n",
            "line comment after a command": f"set(x 1) # {FAKE}\n",
            "bracket comment": f"#[[\n{FAKE}\n]]\n",
            "bracket comment with equals": f"#[==[\n{FAKE}\n]]\n]=]\n]==]\n",
            "quoted argument": f'set(x "\n{FAKE}\n")\n',
            "quoted argument with escaped quotes": f'set(x "\\"\n{FAKE}\\"")\n',
            "bracket argument": f"set(x [[\n{FAKE}\n]])\n",
            "bracket argument with equals": f"set(x [=[\n]]\n{FAKE}\n]=])\n",
        }
        for label, text in cases.items():
            with self.subTest(label):
                self.assertNotIn("fetchcontent_declare", names(text))

    def test_bracket_close_must_match_the_opener(self) -> None:
        (command,) = cc.tokenize("set(x [=[a]]b]==]c]=])\n")
        self.assertEqual(command.arguments[1], cc.Argument("a]]b]==]c", cc.BRACKET))

    def test_a_spaced_hash_bracket_is_a_line_comment(self) -> None:
        # `# [[` is a line comment, so the next line is real code.
        self.assertEqual(names(f"# [[\n{FAKE}\n# ]]\n"), ["fetchcontent_declare"])

    def test_hash_inside_arguments_is_text(self) -> None:
        (command,) = cc.tokenize('set(x "a#b" [[c#d]] e\\#f)\n')
        self.assertEqual(command.values(), ["x", "a#b", "c#d", "e#f"])

    def test_escapes_and_continuations(self) -> None:
        (command,) = cc.tokenize('set(x a\\(b\\) "c\\\nd" "e\\"f" g\\;h)\n')
        self.assertEqual(command.values(), ["x", "a(b)", "cd", 'e"f', "g\\;h"])

    def test_an_escaped_dollar_keeps_its_escape(self) -> None:
        (command,) = cc.tokenize('set(x "\\${A}" \\${B} ${C})\n')
        self.assertEqual(command.values(), ["x", "\\${A}", "\\${B}", "${C}"])

    def test_nested_parentheses_stay_in_one_command(self) -> None:
        (command,) = cc.tokenize("if((A OR B) AND (C))\n")
        self.assertEqual(command.name, "if")
        self.assertEqual(command.values(), ["(", "A", "OR", "B", ")", "AND", "(", "C", ")"])

    def test_comments_inside_an_argument_list(self) -> None:
        (command,) = cc.tokenize("set(x # GIT_TAG fake )\n  #[[ ) ]] y)\n")
        self.assertEqual(command.values(), ["x", "y"])

    def test_line_numbers_follow_the_source(self) -> None:
        commands = cc.tokenize('#[[\n\n]]\nset(a "\n")\nset(b 1)\n')
        self.assertEqual([command.line for command in commands], [4, 6])

    def test_malformed_input_raises(self) -> None:
        for label, text in {
            "unterminated quote": 'set(x "abc)\n',
            "unterminated bracket argument": "set(x [=[abc]])\n",
            "unterminated bracket comment": "#[[ abc\nset(x 1)\n",
            "unterminated argument list": "set(x (a)\n",
            "text that is not a command": "set(x 1)\nstray\n",
            "two commands on one line": "set(x 1) set(y 2)\n",
        }.items():
            with self.subTest(label), self.assertRaises(cc.CMakeSyntaxError):
                cc.tokenize(text)


class RepositoryTests(unittest.TestCase):
    def test_the_repository_cmake_parses(self) -> None:
        for rel in ("tools/cmake/PulpGpuRuntime.cmake",
                    "tools/cmake/PulpGpuRuntimeRecord.cmake", "tools/cmake/PulpVellumLock.cmake"):
            path = ROOT / rel
            with self.subTest(path.name):
                self.assertTrue(cc.tokenize(path.read_text(encoding="utf-8")))


if __name__ == "__main__":
    unittest.main()
