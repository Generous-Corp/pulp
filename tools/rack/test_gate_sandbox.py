#!/usr/bin/env python3
"""The behaviour gate runs model-written C++, so it runs fenced in.

gate_launch() is what both the preflight and the per-module gate use. These
checks drive it with a small canary binary instead of a generated module, so
they need clang and (on macOS) sandbox-exec, but no Rack install.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import generate

CANARY = r'''
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
extern char** environ;
int main(int argc, char** argv) {
    const char* what = argv[1];
    if (!strcmp(what, "net")) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(443);
        inet_pton(AF_INET, "1.1.1.1", &a.sin_addr);
        return connect(s, (sockaddr*)&a, sizeof a) == 0 ? 0 : 3;
    }
    if (!strcmp(what, "write")) return fopen(argv[2], "w") ? 0 : 3;
    if (!strcmp(what, "spawn")) {
        pid_t pid;
        char* av[] = {(char*)"/bin/echo", (char*)"hi", nullptr};
        return posix_spawn(&pid, "/bin/echo", nullptr, nullptr, av, environ) == 0
                   ? 0 : 3;
    }
    return 1;
}
'''


class GateEnvironmentTests(unittest.TestCase):
    def test_secrets_and_home_do_not_reach_the_gate(self):
        with tempfile.TemporaryDirectory() as scratch:
            env = {"PATH": "/usr/bin:/bin", "HOME": "/Users/someone",
                   "ANTHROPIC_API_KEY": "sk-secret", "OPENAI_API_KEY": "sk-2",
                   "FORGE_CLAUDE_BIN": "/opt/claude", "LANG": "en_US.UTF-8"}
            with mock.patch.dict(os.environ, env, clear=True):
                argv, gate_env = generate.gate_launch("/bin/true", scratch)
            self.assertNotIn("ANTHROPIC_API_KEY", gate_env)
            self.assertNotIn("OPENAI_API_KEY", gate_env)
            self.assertNotIn("FORGE_CLAUDE_BIN", gate_env)
            self.assertEqual(gate_env["HOME"], os.path.realpath(scratch))
            self.assertEqual(gate_env["PATH"], "/usr/bin:/bin")
            self.assertEqual(gate_env["LANG"], "en_US.UTF-8")
            # The library path still reaches the gate, by one route or the
            # other: inside the sandbox via env, or directly without one.
            self.assertTrue(
                "DYLD_LIBRARY_PATH=" + generate.SDK in argv
                or gate_env.get("DYLD_LIBRARY_PATH") == generate.SDK)


@unittest.skipUnless(sys.platform == "darwin" and
                     os.path.exists(generate.SANDBOX_EXEC) and
                     shutil.which("clang++"),
                     "needs macOS sandbox-exec and clang++")
class GateSandboxTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        source = os.path.join(cls.build.name, "canary.cpp")
        with open(source, "w") as f:
            f.write(CANARY)
        cls.exe = os.path.join(cls.build.name, "canary")
        subprocess.run(["clang++", "-std=c++20", "-O1", source, "-o", cls.exe],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def run_canary(self, scratch, *args):
        argv, env = generate.gate_launch(self.exe, scratch)
        return subprocess.run(argv + list(args), capture_output=True,
                              text=True, env=env, timeout=30).returncode

    def test_writes_stay_in_scratch(self):
        with tempfile.TemporaryDirectory() as scratch, \
                tempfile.TemporaryDirectory() as elsewhere:
            inside = os.path.join(scratch, "ok.txt")
            outside = os.path.join(elsewhere, "escaped.txt")
            self.assertEqual(self.run_canary(scratch, "write", inside), 0)
            self.assertTrue(os.path.exists(inside))
            self.assertNotEqual(self.run_canary(scratch, "write", outside), 0)
            self.assertFalse(os.path.exists(outside))

    def test_no_network(self):
        with tempfile.TemporaryDirectory() as scratch:
            self.assertNotEqual(self.run_canary(scratch, "net"), 0)

    def test_no_child_processes(self):
        with tempfile.TemporaryDirectory() as scratch:
            self.assertNotEqual(self.run_canary(scratch, "spawn"), 0)


if __name__ == "__main__":
    unittest.main()
