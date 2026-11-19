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


def live(text: str) -> list[str]:
    return [command.name for command in cc.executable_commands(text)]


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
                self.assertNotIn("fetchcontent_declare", live(text))

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
            "unbalanced block": "if(A)\nset(x 1)\n",
            "stray endif": "endif()\n",
        }.items():
            with self.subTest(label), self.assertRaises(cc.CMakeSyntaxError):
                cc.executable_commands(text)


class ExecutableTests(unittest.TestCase):
    def test_constant_false_branches_are_dropped(self) -> None:
        for label, text in {
            "if(FALSE)": f"if(FALSE)\n{FAKE}\nendif()\n",
            "if(0)": f"if(0)\n{FAKE}\nendif()\n",
            "if(NOT ON)": f"if(NOT ON)\n{FAKE}\nendif()\n",
            "if quoted empty": f'if("")\n{FAKE}\nendif()\n',
            "-NOTFOUND": f"if(X-NOTFOUND)\n{FAKE}\nendif()\n",
            "else after if(TRUE)": f"if(TRUE)\nset(x 1)\nelse()\n{FAKE}\nendif()\n",
            "elseif after if(1)": f"if(1)\nset(x 1)\nelseif(A)\n{FAKE}\nendif()\n",
            "elseif(OFF)": f"if(A)\nset(x 1)\nelseif(OFF)\n{FAKE}\nendif()\n",
            "while(FALSE)": f"while(FALSE)\n{FAKE}\nendwhile()\n",
            "nested inside dead": f"if(NO)\nif(A)\n{FAKE}\nendif()\nendif()\n",
            "function body": f"function(f)\n{FAKE}\nendfunction()\n",
            "macro body": f"macro(m)\n{FAKE}\nendmacro()\n",
            "after top-level return": f"return()\n{FAKE}\n",
        }.items():
            with self.subTest(label):
                self.assertIn("fetchcontent_declare", names(text))
                self.assertNotIn("fetchcontent_declare", live(text))

    def test_variable_conditions_stay_live(self) -> None:
        for label, text in {
            "if(VAR)": f"if(VAR)\n{FAKE}\nendif()\n",
            "else of a variable": f"if(VAR)\nset(x 1)\nelse()\n{FAKE}\nendif()\n",
            "if(${X})": f"if(${{X}})\n{FAKE}\nendif()\n",
            "else after if(FALSE)": f"if(FALSE)\nset(x 1)\nelse()\n{FAKE}\nendif()\n",
            "conditional return": f"if(A)\nreturn()\nendif()\n{FAKE}\n",
            "foreach body": f"foreach(x a b)\n{FAKE}\nendforeach()\n",
        }.items():
            with self.subTest(label):
                self.assertIn("fetchcontent_declare", live(text))

    def test_blocks_record_the_branch(self) -> None:
        commands = cc.executable_commands("if(A)\nset(x 1)\nelse()\nset(y 2)\nendif()\n")
        opener, first, second = commands
        self.assertEqual(opener.opens, 0)
        self.assertEqual(first.blocks, ((0, 0),))
        self.assertEqual(second.blocks, ((0, 1),))

    def test_the_repository_cmake_parses(self) -> None:
        for rel in ("tools/cmake/PulpGpuRuntime.cmake",
                    "tools/cmake/PulpGpuRuntimeRecord.cmake", "tools/cmake/PulpVellumLock.cmake"):
            path = ROOT / rel
            with self.subTest(path.name):
                self.assertTrue(cc.executable_commands(path.read_text(encoding="utf-8")))


if __name__ == "__main__":
    unittest.main()
