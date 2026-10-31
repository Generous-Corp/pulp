#!/usr/bin/env python3
"""Behavioral contract for the Mac Pro Windows snapshot-reset supervisor.

The supervisor runs against fake `qm`, `gh`, `ssh`, governor and `flock`
executables that share one JSON state file, so each test observes what the
supervisor did to the VM and to the GitHub registrations rather than grepping
its source.
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import signal
import textwrap
import time
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
# Repository-relative literals, so script_test_inputs.py records them as this
# test's inputs and a change to any of them selects the test.
SCRIPT = ROOT / "tools/ci/proxmox-ephemeral-runner-windows.sh"
SERVICE = ROOT / "tools/ci/pulp-windows-ephemeral-pool@.service"
HEALTH = ROOT / "tools/ci/proxmox-host-health.sh"
GOLDEN = "ci-runner-golden-20990101"
JIT_SECRET = "JITCONFIG-SECRET-VALUE"


# Shared by every fake: the supervisor runs the guest's runner in the background
# while it polls GitHub, so each state update is locked and replaced atomically.
STATE_HELPERS = r'''
import contextlib, fcntl, json, os, re, sys, time
state_path = os.environ["FAKE_STATE"]

@contextlib.contextmanager
def locked_state():
    with open(state_path + ".lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        with open(state_path) as handle:
            state = json.load(handle)
        yield state
        tmp = state_path + ".tmp"
        with open(tmp, "w") as handle:
            json.dump(state, handle)
        os.replace(tmp, state_path)
'''

FAKE_QM = STATE_HELPERS + r'''
args = sys.argv[1:]
rc = 0
with locked_state() as state:
    state["qm"].append(args)
    cmd = args[0]
    if cmd == "status":
        print("status: " + state["vm"])
    elif cmd == "listsnapshot":
        for name in state["snapshots"]:
            print(" `-> " + name + "  2099-01-01 00:00:00  golden")
        print("  `-> current  You are here!")
    elif cmd in ("shutdown", "stop"):
        state["vm"] = "stopped"
    elif cmd == "rollback":
        if state["vm"] != "stopped" or args[2] not in state["snapshots"]:
            rc = 1
        else:
            state["rollbacks"] += 1
            state["events"].append("rollback")
    elif cmd == "start":
        state["vm"] = "running"
    elif cmd == "guest":
        if state["vm"] == "running":
            print(json.dumps([
                {"name": "Loopback Pseudo-Interface 1",
                 "ip-addresses": [{"ip-address-type": "ipv4", "ip-address": "127.0.0.1"}]},
                {"name": "Ethernet",
                 "ip-addresses": [{"ip-address-type": "ipv4", "ip-address": "169.254.3.4"},
                                  {"ip-address-type": "ipv4", "ip-address": "192.0.2.21"}]},
            ]))
        else:
            rc = 2
sys.exit(rc)
'''

FAKE_GH = STATE_HELPERS + r'''
args = sys.argv[1:]
assert args[0] == "api", args
method = args[args.index("--method") + 1] if "--method" in args else "GET"
path = next(a for a in args[1:] if "/" in a and not a.startswith("-"))
rc = 0
out = ""
with locked_state() as state:
    state["gh"].append(args)
    if path.endswith("/generate-jitconfig"):
        with open(args[args.index("--input") + 1]) as handle:
            state["jit_requests"].append(json.load(handle))
        if state["mint_ok"]:
            out = os.environ["FAKE_JIT"]
        else:
            rc = 1
    elif re.search(r"/actions/runners\?", path):
        out = "\n".join("%s\t%s\t%s\t%s" % (r["id"], r["name"], r["status"], str(r["busy"]).lower())
                        for r in state["runners"])
    elif re.search(r"/actions/runners/\d+$", path):
        rid = int(path.rsplit("/", 1)[1])
        match = [r for r in state["runners"] if r["id"] == rid]
        if method == "DELETE":
            if state.get("delete_422") or (match and match[0]["busy"]):
                sys.stderr.write("HTTP 422: runner is busy\n")
                rc = 1
            else:
                state["runners"] = [r for r in state["runners"] if r["id"] != rid]
                state["deleted"].append(rid)
        elif not match:
            sys.stderr.write("HTTP 404: Not Found\n")
            rc = 1
        else:
            r = match[0]
            # `reread_busy`: the runner list still says idle, but GitHub has
            # just assigned a job, which a direct read of the runner shows.
            busy = True if state.get("reread_busy") else r["busy"]
            out = "%s\t%s\t%s" % (r["id"], r["name"], str(busy).lower())
    elif "/actions/runs?" in path:
        out = ""
    else:
        sys.stderr.write("unexpected path %s\n" % path)
        rc = 1
if out:
    print(out)
sys.exit(rc)
'''

FAKE_PVESH = STATE_HELPERS + r'''
with locked_state() as state:
    tasks = state["tasks"]
print(json.dumps(tasks))
'''

FAKE_JOURNALCTL = r'''
print("Accepted publickey for root from 192.0.2.174 port 59530 ssh2: RSA SHA256:x")
print("Connection closed by 192.0.2.9 port 22")
'''

# The guest. `run.cmd` registers the JIT runner as online, takes a job, and
# deregisters itself, the way an ephemeral runner does after one job.
FAKE_SSH = STATE_HELPERS + r'''
EXTERNAL_STOP = (
    "import json, os, time, fcntl\n"
    "path = os.environ['FAKE_STATE']\n"
    "def update(change):\n"
    "    with open(path + '.lock', 'a') as lock:\n"
    "        fcntl.flock(lock, fcntl.LOCK_EX)\n"
    "        state = json.load(open(path))\n"
    "        change(state)\n"
    "        json.dump(state, open(path + '.tmp', 'w'))\n"
    "        os.replace(path + '.tmp', path)\n"
    "time.sleep(1)\n"
    "def stop(state):\n"
    "    state['vm'] = 'stopped'\n"
    "    state['tasks'].append({'type': 'qmshutdown', 'user': 'root@pam', 'starttime': int(time.time())})\n"
    "    state['events'].append('external-stop')\n"
    "update(stop)\n"
    "time.sleep(1.5)\n"
    "def lost(state):\n"
    "    state['runners'] = [r for r in state['runners'] if r['id'] != 4242]\n"
    "update(lost)\n"
)
FINISH_JOB = (
    "import json, os, sys, time, fcntl\n"
    "time.sleep(float(sys.argv[1]))\n"
    "path = os.environ['FAKE_STATE']\n"
    "with open(path + '.lock', 'a') as lock:\n"
    "    fcntl.flock(lock, fcntl.LOCK_EX)\n"
    "    state = json.load(open(path))\n"
    "    state['runners'] = [r for r in state['runners'] if r['id'] != 4242]\n"
    "    state['jobs_run'] += 1\n"
    "    state['events'].append('job-done')\n"
    "    json.dump(state, open(path + '.tmp', 'w'))\n"
    "    os.replace(path + '.tmp', path)\n"
)
args = sys.argv[1:]
command = args[-1]
with locked_state() as state:
    state["ssh"].append(args)
    behavior = state["runner_behavior"]
if command == "exit 0":
    sys.exit(0)
if "WriteAllText" in command:
    jit = sys.stdin.read().strip()
    with locked_state() as state:
        state["guest_jit"] = jit
    sys.exit(0)
if "run.cmd" in command:
    if behavior == "assigned-while-idle":
        # Listed idle past the limit while GitHub is assigning it a job; the
        # job then runs to completion.
        with locked_state() as state:
            name = state["jit_requests"][-1]["name"]
            state["runners"].append({"id": 4242, "name": name, "status": "online", "busy": False})
        time.sleep(float(os.environ.get("FAKE_JOB_SECONDS", "3")))
        with locked_state() as state:
            state["runners"] = [r for r in state["runners"] if r["id"] != 4242]
            state["jobs_run"] += 1
            state["events"].append("job-done")
        sys.exit(0)
    if behavior == "idle-forever":
        # Online and never assigned a job, as a stale JIT session looks.
        with locked_state() as state:
            name = state["jit_requests"][-1]["name"]
            state["runners"].append({"id": 4242, "name": name, "status": "online", "busy": False})
        time.sleep(120)
        sys.exit(0)
    if behavior == "never-online":
        time.sleep(30)
        sys.exit(1)
    if behavior == "external-stop":
        # Someone else stops the VM mid-job; GitHub notices the lost runner a
        # little later.
        with locked_state() as state:
            name = state["jit_requests"][-1]["name"]
            state["runners"].append({"id": 4242, "name": name, "status": "online", "busy": True})
        import subprocess
        subprocess.Popen(
            [sys.executable, "-c", EXTERNAL_STOP],
            start_new_session=True,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        time.sleep(120)
        sys.exit(0)
    if behavior == "long-job":
        # The job runs in the guest, not in this SSH session: killing the
        # session (as the supervisor does on SIGTERM) must not end it. A
        # detached child finishes it after FAKE_JOB_SECONDS.
        with locked_state() as state:
            name = state["jit_requests"][-1]["name"]
            state["runners"].append({"id": 4242, "name": name, "status": "online", "busy": True})
        import subprocess
        subprocess.Popen(
            [sys.executable, "-c", FINISH_JOB, os.environ.get("FAKE_JOB_SECONDS", "2")],
            start_new_session=True,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        time.sleep(120)
        sys.exit(0)
    with locked_state() as state:
        name = state["jit_requests"][-1]["name"]
        state["runners"].append({"id": 4242, "name": name, "status": "online", "busy": False})
    time.sleep(0.3)
    with locked_state() as state:
        for r in state["runners"]:
            if r["id"] == 4242:
                r["busy"] = True
    time.sleep(0.3)
    with locked_state() as state:
        state["runners"] = [r for r in state["runners"] if r["id"] != 4242]
        state["jobs_run"] += 1
    sys.exit(0)
sys.exit(3)
'''


def write_exec(path: pathlib.Path, body: str, interpreter: str) -> None:
    path.write_text(f"#!{interpreter}\n" + body, encoding="utf-8")
    path.chmod(0o755)


class WindowsSupervisorTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = pathlib.Path(self._tmp.name)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        python = shutil.which("python3")
        assert python
        write_exec(self.bin / "qm", FAKE_QM, python)
        write_exec(self.bin / "gh", FAKE_GH, python)
        write_exec(self.bin / "ssh", FAKE_SSH, python)
        write_exec(self.bin / "flock", "exit 0\n", "/bin/bash")
        write_exec(self.bin / "pvesh", FAKE_PVESH, python)
        write_exec(self.bin / "journalctl", FAKE_JOURNALCTL, python)
        write_exec(self.bin / "logger", 'echo "$*" >> "$FAKE_SYSLOG"\n', "/bin/bash")
        write_exec(
            self.bin / "governor",
            textwrap.dedent(
                """\
                [ "$1" = can-start ] || exit 2
                [ "$(cat "$FAKE_GOVERNOR")" = admit ] && { echo "ADMIT $2"; exit 0; }
                echo "REFUSE $2 (4c/9216M)"; exit 1
                """
            ),
            "/bin/bash",
        )
        self.governor = self.tmp / "governor-verdict"
        self.governor.write_text("admit", encoding="utf-8")
        self.pat = self.tmp / "pat"
        self.pat.write_text("ghp_fake", encoding="utf-8")
        self.state_path = self.tmp / "state.json"
        self.state = {
            "vm": "stopped",
            "snapshots": [GOLDEN],
            "rollbacks": 0,
            "runners": [],
            "deleted": [],
            "jit_requests": [],
            "mint_ok": True,
            "runner_behavior": "one-job",
            "jobs_run": 0,
            "events": [],
            "tasks": [],
            "guest_jit": "",
            "qm": [],
            "gh": [],
            "ssh": [],
        }

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def base_env(self) -> dict[str, str]:
        return {
            "PATH": f"{self.bin}:{os.environ['PATH']}",
            "HOME": str(self.tmp),
            "FAKE_STATE": str(self.state_path),
            "FAKE_GOVERNOR": str(self.governor),
            "FAKE_JIT": JIT_SECRET,
            "PULP_WINDOWS_VMID": "300",
            "PULP_WINDOWS_GOLDEN_SNAPSHOT": GOLDEN,
            "PULP_RUNNER_PAT_FILE": str(self.pat),
            "PULP_WINDOWS_SKIP_CREDENTIAL_MODE_CHECK": "1",
            "PULP_WINDOWS_FLOCK": str(self.bin / "flock"),
            "PULP_WINDOWS_GOVERNOR": str(self.bin / "governor"),
            "PULP_WINDOWS_STATE_DIR": str(self.tmp / "state"),
            "PULP_WINDOWS_LOCK_DIR": str(self.tmp),
            "PULP_WINDOWS_POLL_SECONDS": "0.1",
            "PULP_WINDOWS_RUNNER_HEARTBEAT_INTERVAL_SECONDS": "0.1",
            "PULP_WINDOWS_RUNNER_READY_TIMEOUT_SECONDS": "3",
            "PULP_WINDOWS_GUEST_BOOT_TIMEOUT_SECONDS": "5",
            "PULP_WINDOWS_BUSY_WAIT_SECONDS": "0",
            "PULP_GOVERNOR_REFUSAL_BACKOFF_SECONDS": "0",
            "PULP_WINDOWS_BUSY_POLL_SECONDS": "0.2",
            "PULP_WINDOWS_VM_WATCH_SECONDS": "0.2",
            "PULP_WINDOWS_PVESH": str(self.bin / "pvesh"),
            "PULP_WINDOWS_JOURNALCTL": str(self.bin / "journalctl"),
            "PULP_WINDOWS_LOGGER": str(self.bin / "logger"),
            "FAKE_SYSLOG": str(self.tmp / "syslog"),
        }

    def run_supervisor(self, *args: str, **env_overrides: str) -> subprocess.CompletedProcess:
        self.state_path.write_text(json.dumps(self.state), encoding="utf-8")
        env = self.base_env()
        env.update(env_overrides)
        result = subprocess.run(
            ["/bin/bash", str(SCRIPT), *args],
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=60,
        )
        self.state = json.loads(self.state_path.read_text(encoding="utf-8"))
        return result

    def start_supervisor(self, **env_overrides: str) -> subprocess.Popen:
        self.state_path.write_text(json.dumps(self.state), encoding="utf-8")
        env = self.base_env()
        env.update(env_overrides)
        return subprocess.Popen(
            ["/bin/bash", str(SCRIPT)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
        )

    def wait_for_busy_runner(self, deadline_seconds: float = 20) -> None:
        deadline = time.monotonic() + deadline_seconds
        while time.monotonic() < deadline:
            state = json.loads(self.state_path.read_text(encoding="utf-8"))
            if any(r["busy"] for r in state["runners"]):
                return
            time.sleep(0.1)
        self.fail("the fake guest never reported a busy runner")

    def qm_verbs(self) -> list[str]:
        return [call[0] for call in self.state["qm"] if call[0] in ("rollback", "start")]

    def test_one_job_runs_between_two_rollbacks(self) -> None:
        result = self.run_supervisor()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.state["jobs_run"], 1)
        self.assertEqual(self.qm_verbs(), ["rollback", "start", "rollback"])
        self.assertEqual(self.state["vm"], "stopped")
        self.assertIn(["rollback", "300", GOLDEN], self.state["qm"])
        self.assertIn("192.0.2.21", result.stdout)

    def test_jit_config_travels_on_stdin_never_argv(self) -> None:
        result = self.run_supervisor()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.state["guest_jit"], JIT_SECRET)
        for call in self.state["ssh"] + self.state["gh"] + self.state["qm"]:
            self.assertNotIn(JIT_SECRET, " ".join(call))
        self.assertNotIn(JIT_SECRET, result.stdout + result.stderr)

    def test_registration_is_repository_scoped_with_exact_labels(self) -> None:
        result = self.run_supervisor()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        request = self.state["jit_requests"][0]
        self.assertEqual(request["runner_group_id"], 1)
        self.assertEqual(
            request["labels"],
            ["self-hosted", "Windows", "X64", "pulp-build-windows-x64", "pulp-host-macpro"],
        )
        self.assertTrue(request["name"].startswith("pulp-win-ephemeral-300-"))
        mint = [c for c in self.state["gh"] if any(a.endswith("generate-jitconfig") for a in c)]
        self.assertEqual(mint[0][mint[0].index("--method") + 2], "repos/Generous-Corp/pulp/actions/runners/generate-jitconfig")

    def test_guest_host_key_is_pinned_to_a_per_vm_alias(self) -> None:
        result = self.run_supervisor()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        first = " ".join(self.state["ssh"][0])
        self.assertIn("HostKeyAlias=pulp-windows-vm-300", first)
        self.assertIn("UserKnownHostsFile=" + str(self.tmp / "state" / "known_hosts"), first)

    def test_governor_refusal_exits_75_without_starting_the_vm(self) -> None:
        self.governor.write_text("refuse", encoding="utf-8")
        result = self.run_supervisor()
        self.assertEqual(result.returncode, 75, result.stdout + result.stderr)
        self.assertNotIn("start", self.qm_verbs())
        self.assertEqual(self.state["jit_requests"], [])
        self.assertIn("REFUSED by governor", result.stdout)

    def test_busy_slot_registration_is_never_reset_under_its_job(self) -> None:
        self.state["vm"] = "running"
        self.state["runners"] = [
            {"id": 7, "name": "pulp-win-ephemeral-300-old", "status": "online", "busy": True}
        ]
        result = self.run_supervisor()
        self.assertEqual(result.returncode, 75, result.stdout + result.stderr)
        self.assertEqual(self.state["vm"], "running")
        self.assertEqual(self.state["rollbacks"], 0)
        self.assertEqual(self.state["deleted"], [])

    def test_idle_leftover_registration_is_fenced_before_the_reset(self) -> None:
        self.state["vm"] = "running"
        self.state["runners"] = [
            {"id": 7, "name": "pulp-win-ephemeral-300-old", "status": "offline", "busy": False},
            {"id": 8, "name": "someone-else-300-x", "status": "online", "busy": False},
        ]
        result = self.run_supervisor()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.state["deleted"], [7])
        self.assertEqual(self.state["jobs_run"], 1)

    def test_missing_golden_snapshot_refuses_before_touching_the_vm(self) -> None:
        self.state["snapshots"] = ["something-else"]
        result = self.run_supervisor()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("has no snapshot", result.stdout)
        self.assertEqual(self.qm_verbs(), [])

    def test_automatic_routing_labels_are_refused_at_repository_scope(self) -> None:
        result = self.run_supervisor(
            PULP_WINDOWS_RUNNER_LABELS="self-hosted,Windows,X64,pulp-auto-linux-x64"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("organization runner group", result.stdout)
        self.assertEqual(self.state["qm"], [])

    def test_runner_that_never_comes_online_still_resets_the_vm(self) -> None:
        self.state["runner_behavior"] = "never-online"
        result = self.run_supervisor()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("never became visible", result.stdout)
        self.assertEqual(self.qm_verbs(), ["rollback", "start", "rollback"])
        self.assertEqual(self.state["vm"], "stopped")

    def test_sigterm_during_a_job_drains_it_before_the_reset(self) -> None:
        self.state["runner_behavior"] = "long-job"
        proc = self.start_supervisor(PULP_WINDOWS_BUSY_WAIT_SECONDS="30", FAKE_JOB_SECONDS="2")
        self.wait_for_busy_runner()
        proc.send_signal(signal.SIGTERM)
        output, _ = proc.communicate(timeout=60)
        self.state = json.loads(self.state_path.read_text(encoding="utf-8"))
        self.assertEqual(self.state["jobs_run"], 1, output)
        # Pre-job reset, the job, then the post-job reset: never a rollback
        # while the job was still running.
        self.assertEqual(self.state["events"], ["rollback", "job-done", "rollback"], output)
        self.assertEqual(self.state["vm"], "stopped", output)
        self.assertIn("is running a job; waiting", output)

    def test_sigterm_with_a_job_outlasting_the_wait_leaves_the_vm_running(self) -> None:
        self.state["runner_behavior"] = "long-job"
        proc = self.start_supervisor(PULP_WINDOWS_BUSY_WAIT_SECONDS="0", FAKE_JOB_SECONDS="60")
        self.wait_for_busy_runner()
        proc.send_signal(signal.SIGTERM)
        output, _ = proc.communicate(timeout=60)
        self.state = json.loads(self.state_path.read_text(encoding="utf-8"))
        self.assertNotEqual(proc.returncode, 0, output)
        self.assertEqual(self.state["events"], ["rollback"], output)
        self.assertEqual(self.state["vm"], "running", output)
        self.assertIn("leaving VM 300 running", output)

    def test_external_vm_stop_is_named_fenced_and_reset(self) -> None:
        self.state["runner_behavior"] = "external-stop"
        started = time.monotonic()
        result = self.run_supervisor(PULP_WINDOWS_BUSY_WAIT_SECONDS="30")
        elapsed = time.monotonic() - started
        self.assertNotEqual(result.returncode, 0, result.stdout)
        # Detected by the VM watch, not by the 3-minute SSH keepalive.
        self.assertLess(elapsed, 30, result.stdout)
        self.assertIn("VM 300 stopped externally during the job", result.stdout)
        self.assertIn("task=qmshutdown by root@pam", result.stdout)
        self.assertIn("root-ssh-from=192.0.2.174", result.stdout)
        self.assertNotIn("192.0.2.9", result.stdout)
        record = (self.tmp / "state" / "external-stops.log").read_text(encoding="utf-8")
        self.assertIn("vm=300 status=stopped", record)
        self.assertIn("root-ssh-from=192.0.2.174", record)
        self.assertIn("stopped externally", (self.tmp / "syslog").read_text(encoding="utf-8"))
        # The lost registration is fenced and the VM is reset for the next job.
        self.assertEqual(self.state["events"], ["rollback", "external-stop", "rollback"])
        self.assertEqual(self.state["runners"], [])

    def test_a_normal_job_records_no_external_stop(self) -> None:
        result = self.run_supervisor()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("stopped externally", result.stdout)
        self.assertFalse((self.tmp / "state" / "external-stops.log").exists())

    def test_a_runner_idle_past_the_limit_is_fenced_and_re_registered(self) -> None:
        self.state["runner_behavior"] = "idle-forever"
        result = self.run_supervisor(PULP_WINDOWS_RUNNER_IDLE_RECYCLE_SECONDS="1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("recycling a JIT runner that stayed idle past 1s", result.stdout)
        self.assertIn("fence: deregistered idle runner", result.stdout)
        self.assertEqual(self.state["deleted"], [4242])
        self.assertEqual(self.state["jobs_run"], 0)
        self.assertEqual(self.qm_verbs(), ["rollback", "start", "rollback"])
        self.assertEqual(self.state["vm"], "stopped")

    def test_a_busy_runner_is_never_recycled_for_idleness(self) -> None:
        self.state["runner_behavior"] = "long-job"
        result = self.run_supervisor(PULP_WINDOWS_RUNNER_IDLE_RECYCLE_SECONDS="1",
                                     PULP_WINDOWS_BUSY_WAIT_SECONDS="30", FAKE_JOB_SECONDS="3")
        self.assertNotIn("recycling a JIT runner", result.stdout)
        self.assertEqual(self.state["deleted"], [])
        self.assertEqual(self.state["jobs_run"], 1, result.stdout)

    def assert_assigned_runner_survives_the_idle_limit(self, **flags: bool) -> None:
        self.state["runner_behavior"] = "assigned-while-idle"
        self.state.update(flags)
        result = self.run_supervisor(PULP_WINDOWS_RUNNER_IDLE_RECYCLE_SECONDS="1",
                                     FAKE_JOB_SECONDS="3")
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertNotIn("recycling a JIT runner", output)
        self.assertEqual(self.state["deleted"], [], output)
        self.assertEqual(self.state["jobs_run"], 1, output)
        self.assertEqual(self.state["events"], ["rollback", "job-done", "rollback"], output)
        # Control: the idle limit was reached and the fence was tried.
        self.assertIn("fence:", output)

    def test_a_runner_the_fence_reads_as_busy_keeps_its_job(self) -> None:
        self.assert_assigned_runner_survives_the_idle_limit(reread_busy=True)

    def test_a_runner_whose_delete_is_refused_keeps_its_job(self) -> None:
        self.assert_assigned_runner_survives_the_idle_limit(delete_422=True)

    def test_idle_recycle_zero_disables_it(self) -> None:
        self.state["runner_behavior"] = "idle-forever"
        proc = self.start_supervisor(PULP_WINDOWS_RUNNER_IDLE_RECYCLE_SECONDS="0")
        time.sleep(4)
        self.state = json.loads(self.state_path.read_text(encoding="utf-8"))
        proc.terminate()
        output, _ = proc.communicate(timeout=60)
        self.assertEqual(self.state["deleted"], [], output)
        self.assertNotIn("recycling a JIT runner", output)

    def test_a_malformed_idle_limit_refuses_before_touching_the_vm(self) -> None:
        result = self.run_supervisor(PULP_WINDOWS_RUNNER_IDLE_RECYCLE_SECONDS="45m")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("whole number of seconds", result.stdout)
        self.assertEqual(self.state["qm"], [])

    def test_keep_leaves_the_vm_up(self) -> None:
        result = self.run_supervisor("--keep")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.qm_verbs(), ["rollback", "start"])
        self.assertEqual(self.state["vm"], "running")


class WindowsDeploymentTests(unittest.TestCase):
    def test_stop_timeout_outlasts_the_busy_wait(self) -> None:
        # systemd must not SIGKILL a supervisor that is still draining a job.
        busy_wait = int(re.search(
            r'BUSY_WAIT_SECONDS="\$\{PULP_WINDOWS_BUSY_WAIT_SECONDS:-(\d+)\}"',
            SCRIPT.read_text(encoding="utf-8")).group(1))
        stop_timeout = int(re.search(
            r"(?m)^TimeoutStopSec=(\d+)$", SERVICE.read_text(encoding="utf-8")).group(1))
        self.assertGreaterEqual(stop_timeout, busy_wait + 300)

    def test_unit_runs_the_installed_supervisor_with_a_restart_ceiling(self) -> None:
        unit = SERVICE.read_text(encoding="utf-8")
        self.assertIn("ExecStart=/usr/local/sbin/pulp-windows-ephemeral-runner.sh", unit)
        self.assertIn("EnvironmentFile=/etc/pulp/windows-runner-%i.env", unit)
        self.assertIn("StartLimitBurst=40", unit.split("[Service]")[0])

    def test_host_health_installs_and_watches_the_windows_slot(self) -> None:
        manifest = subprocess.run(
            ["/bin/bash", str(HEALTH), "--manifest"], capture_output=True, text=True, encoding="utf-8", check=True
        ).stdout
        self.assertIn("proxmox-ephemeral-runner-windows.sh", manifest)
        self.assertIn("/usr/local/sbin/pulp-windows-ephemeral-runner.sh", manifest)
        self.assertIn("/etc/systemd/system/pulp-windows-ephemeral-pool@.service", manifest)
        self.assertIn("'pulp-windows-ephemeral-pool@*'", HEALTH.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
