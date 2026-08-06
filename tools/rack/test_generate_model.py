#!/usr/bin/env python3
"""Selected-provider protocol checks for Forge's generated-module path."""
from __future__ import annotations

import os
import stat
import tempfile
import unittest
from unittest import mock

import generate


class GenerateModelProtocolTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        contract = os.path.join(self.temp.name, "contract.md")
        with open(contract, "w") as f:
            f.write("contract <!--DSP_VOCABULARY-->")
        self.contract = mock.patch.object(generate, "CONTRACT", contract)
        self.vocabulary = mock.patch.object(
            generate, "dsp_vocabulary", return_value="known dsp")
        self.contract.start()
        self.vocabulary.start()
        self.addCleanup(self.contract.stop)
        self.addCleanup(self.vocabulary.stop)

    def wrapper(self, name: str, body: str) -> str:
        path = os.path.join(self.temp.name, name)
        with open(path, "w") as f:
            f.write("#!/usr/bin/env python3\n" + body)
        os.chmod(path, os.stat(path).st_mode | stat.S_IEXEC)
        return path

    def test_selected_claude_uses_claude_protocol_and_exact_model(self):
        wrapper = self.wrapper("arbitrary-claude-wrapper", r'''
import json
import sys
expected = ["-p", "--strict-mcp-config", "--verbose",
            "--output-format=stream-json", "--include-partial-messages",
            "--model", "claude-opus-5", "--effort", "medium"]
if sys.argv[1:] != expected:
    print("wrong Claude protocol: " + repr(sys.argv[1:]), file=sys.stderr)
    raise SystemExit(64)
if "make a bell" not in sys.stdin.read():
    raise SystemExit(65)
print(json.dumps({"type": "result", "subtype": "success",
                  "result": "CLAUDE MODULE"}))
''')
        env = {"FORGE_MODEL_PROVIDER": "claude",
               "FORGE_CLAUDE_BIN": wrapper,
               "FORGE_CLAUDE_MODEL": "claude-opus-5",
               "FORGE_CLAUDE_REASONING_EFFORT": "medium",
               "FORGE_CODEX_MODEL": "must-not-cross-providers"}
        with mock.patch.dict(os.environ, env, clear=True):
            self.assertEqual(generate.ask_model("make a bell"), "CLAUDE MODULE")

    def test_selected_claude_reasoning_efforts_are_exact_arguments(self):
        wrapper = self.wrapper("claude-effort-wrapper", r'''
import json
import os
import sys
effort = os.environ["FORGE_CLAUDE_REASONING_EFFORT"]
if sys.argv[-2:] != ["--effort", effort]:
    print("wrong Claude effort: " + repr(sys.argv[1:]), file=sys.stderr)
    raise SystemExit(64)
sys.stdin.read()
print(json.dumps({"type": "result", "subtype": "success",
                  "result": "CLAUDE EFFORT"}))
''')
        for effort in ("low", "medium", "high", "max"):
            with self.subTest(effort=effort):
                env = {"FORGE_MODEL_PROVIDER": "claude",
                       "FORGE_CLAUDE_BIN": wrapper,
                       "FORGE_CLAUDE_MODEL": "claude-opus-5",
                       "FORGE_CLAUDE_REASONING_EFFORT": effort}
                with mock.patch.dict(os.environ, env, clear=True):
                    self.assertEqual(generate.ask_model("make a bell"),
                                     "CLAUDE EFFORT")

    def test_selected_codex_uses_codex_protocol_and_exact_model(self):
        wrapper = self.wrapper("arbitrary-codex-wrapper", r'''
import json
import sys
args = sys.argv[1:]
prefix = ["exec", "--model", "gpt-5.6-sol", "-c",
          'model_reasoning_effort="medium"', "--ephemeral", "--sandbox",
          "read-only", "--ignore-user-config", "--ignore-rules", "--color",
          "never", "--skip-git-repo-check", "--json"]
if args[:len(prefix)] != prefix or args[-1] != "-":
    print("wrong Codex protocol: " + repr(args), file=sys.stderr)
    raise SystemExit(64)
answer = args[args.index("--output-last-message") + 1]
if "make a marimba" not in sys.stdin.read():
    raise SystemExit(65)
open(answer, "w").write("CODEX MODULE")
print(json.dumps({"type": "turn.completed", "usage": {}}))
''')
        env = {"FORGE_MODEL_PROVIDER": "codex",
               "FORGE_CODEX_BIN": wrapper,
               "FORGE_CODEX_MODEL": "gpt-5.6-sol",
               "FORGE_CODEX_REASONING_EFFORT": "medium",
               "FORGE_CLAUDE_MODEL": "must-not-cross-providers"}
        with mock.patch.dict(os.environ, env, clear=True):
            self.assertEqual(generate.ask_model("make a marimba"),
                             "CODEX MODULE")

    def test_selected_codex_reasoning_efforts_are_exact_config(self):
        wrapper = self.wrapper("codex-effort-wrapper", r'''
import json
import os
import sys
effort = os.environ["FORGE_CODEX_REASONING_EFFORT"]
expected = ["exec", "--model", "gpt-5.6-sol", "-c",
            'model_reasoning_effort="' + effort + '"', "--ephemeral"]
if sys.argv[1:len(expected) + 1] != expected:
    print("wrong Codex effort: " + repr(sys.argv[1:]), file=sys.stderr)
    raise SystemExit(64)
answer = sys.argv[sys.argv.index("--output-last-message") + 1]
sys.stdin.read()
open(answer, "w").write("CODEX EFFORT")
print(json.dumps({"type": "turn.completed", "usage": {}}))
''')
        for effort in ("low", "medium", "high", "max"):
            with self.subTest(effort=effort):
                env = {"FORGE_MODEL_PROVIDER": "codex",
                       "FORGE_CODEX_BIN": wrapper,
                       "FORGE_CODEX_MODEL": "gpt-5.6-sol",
                       "FORGE_CODEX_REASONING_EFFORT": effort}
                with mock.patch.dict(os.environ, env, clear=True):
                    self.assertEqual(generate.ask_model("make a voice"),
                                     "CODEX EFFORT")

    def test_empty_and_malformed_exact_models_fail_closed(self):
        wrapper = self.wrapper("model-wrapper", "raise SystemExit(99)\n")
        cases = (("claude", "FORGE_CLAUDE_BIN", "FORGE_CLAUDE_MODEL", "   ",
                  "set but empty"),
                 ("claude", "FORGE_CLAUDE_BIN", "FORGE_CLAUDE_MODEL",
                  "--profile surprise", "is malformed"),
                 ("codex", "FORGE_CODEX_BIN", "FORGE_CODEX_MODEL", "   ",
                  "set but empty"),
                 ("codex", "FORGE_CODEX_BIN", "FORGE_CODEX_MODEL",
                  "--profile surprise", "is malformed"))
        for provider, bin_var, model_var, model, expected in cases:
            with self.subTest(provider=provider, model=model):
                env = {"FORGE_MODEL_PROVIDER": provider, bin_var: wrapper,
                       model_var: model}
                with mock.patch.dict(os.environ, env, clear=True):
                    with self.assertRaisesRegex(SystemExit, expected):
                        generate.ask_model("make something")

        for provider, bin_var, effort_var in (
                ("claude", "FORGE_CLAUDE_BIN",
                 "FORGE_CLAUDE_REASONING_EFFORT"),
                ("codex", "FORGE_CODEX_BIN",
                 "FORGE_CODEX_REASONING_EFFORT")):
            for effort in ("", "ultra", "MEDIUM"):
                with self.subTest(provider=provider, effort=effort):
                    env = {"FORGE_MODEL_PROVIDER": provider,
                           bin_var: wrapper, effort_var: effort}
                    with mock.patch.dict(os.environ, env, clear=True):
                        with self.assertRaisesRegex(
                                SystemExit, "must be one of"):
                            generate.ask_model("make something")


if __name__ == "__main__":
    unittest.main()
