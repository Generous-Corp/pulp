#!/usr/bin/env python3
"""Run real G9 production mutations, restoring source/build after every case.

Run from this checkout after other builds stop. Uses the existing configured
Release build; never changes the platform dependency or installs the fixture.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
HOST = ROOT / "core/host/src/isolated_plugin_slot.cpp"
WORKER = ROOT / "tools/clap-worker/clap_worker_main.cpp"
PROTOCOL = ROOT / "core/host/src/isolated_clap_protocol.hpp"
CASES = [
    ("worker-status", WORKER,
     "if (slot->process_status() != host::PluginSlot::ProcessStatus::ok)", "if (false)",
     "Isolated CLAP rejects partial errors and nonfinite output", False),
    ("ordinary-failed", HOST, "    int latency_samples() const override {",
     "    ProcessStatus process_status() const noexcept override {\n"
     "        return session_->fault.load(std::memory_order_acquire) ? ProcessStatus::failed : ProcessStatus::ok;\n"
     "    }\n    int latency_samples() const override {",
     "Isolated CLAP graph fault preserves hosted and built-in siblings with PDC", False),
    ("group-cleanup", HOST,
     "cleanup_session(session, deadline_ns ? Clock::time_point(std::chrono::nanoseconds(deadline_ns))\n"
     "                                         : Clock::now() + std::chrono::seconds(2));",
     "cleanup_session(session, Clock::now());",
     "Isolated CLAP retained group cleans nested descendants after leader exit", False),
    ("callback-wait", HOST,
     "    void process_request(const detail::SlotProcessRequest& request) override {",
     "    void process_request(const detail::SlotProcessRequest& request) override {\n        ::usleep(1);",
     "Isolated CLAP real healthy worker adds one block", False),
    ("parent-validation", PROTOCOL,
     "block.generation == generation && block.sequence == sequence &&",
     "true &&", "Isolated CLAP callback validates adversarial peer publications", True),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build")
    parser.add_argument("--case", choices=[case[0] for case in CASES] + ["construction-retention"])
    args = parser.parse_args()
    build = args.build.resolve()
    evidence = build / "g9-mutations"
    evidence.mkdir(exist_ok=True)
    results = json.loads((evidence / "results.json").read_text(encoding="utf-8")) if args.case and (evidence / "results.json").exists() else {}

    def run(command, label):
        with (evidence / (label + ".log")).open("w", encoding="utf-8") as output:
            return subprocess.run(command, cwd=ROOT, stdout=output,
                                  stderr=subprocess.STDOUT, timeout=600).returncode

    for name, path, needle, replacement, test, protocol in CASES:
        if args.case and args.case != name:
            continue
        original = path.read_bytes()
        source = original.decode()
        if source.count(needle) != 1:
            raise RuntimeError(f"{name}: mutation anchor must occur exactly once")
        target = "pulp-test-isolated-clap-protocol" if protocol else "pulp-test-isolated-clap-worker"
        command = [str(ROOT / "tools/ci/governed-build.sh"), "cmake", "--build", str(build), "--target", target]
        executable = build / "tools/clap-worker" / target
        positive_build = run(command, name + "-positive-build")
        positive = run([str(executable), test], name + "-positive") if positive_build == 0 else None
        if positive_build != 0 or positive != 0:
            raise RuntimeError(f"{name}: positive control failed")
        try:
            path.write_text(source.replace(needle, replacement), encoding="utf-8")
            broken_build = run(command, name + "-broken-build")
            broken = run([str(executable), test], name + "-broken") if broken_build == 0 else None
        finally:
            path.write_bytes(original)
            restored_build = run(command, name + "-restored-build")
            restored = run([str(executable), test], name + "-restored") if restored_build == 0 else None
        results[name] = dict(source_sha256=hashlib.sha256(original).hexdigest(),
                             positive_build=positive_build, positive=positive,
                             broken_build=broken_build, broken=broken,
                             restored_build=restored_build, restored=restored)
        (evidence / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
        print(name, results[name], flush=True)
        if broken_build != 0 or broken in (None, 0) or restored_build != 0 or restored != 0:
            raise RuntimeError(f"{name}: mutation control did not distinguish broken/restored")

    if not args.case or args.case == "construction-retention":
        original = HOST.read_bytes()
        source = original.decode("utf-8")
        needle = "    session->fail(IsolationReason::launch_failed);\n    cleanup_session(session);"
        if source.count(needle) != 1:
            raise RuntimeError("failed-construction anchor must occur exactly once")
        injected = source.replace(needle, "    session->fail(IsolationReason::launch_failed);\n    cleanup_session(session, Clock::now());")
        # Failed handshake cleanup is already owned by ChildProcess. Model a
        # host allocation/thread-construction failure after successful launch.
        launch = "if (launch(session)) {"
        if source.count(launch) != 1:
            raise RuntimeError("successful launch anchor must occur exactly once")
        injected = injected.replace(launch, "if (launch(session) && false) {")
        target = "pulp-test-isolated-clap-construction"
        command = [str(ROOT / "tools/ci/governed-build.sh"), "cmake", "--build", str(build), "--target", target, "pulp-isolated-clap-fixture"]
        directory = build / "tools/clap-worker"
        probe = [str(directory / target), str(directory / "pulp-test-clap-worker-mismatched"),
                 str(directory / "pulp-clap-worker"), str(directory / "pulp-isolated-clap-fixture.clap")]
        # First prove normal failed-handshake cleanup permits healthy creation.
        normal_build = run(command, "construction-normal-build")
        normal = run(probe + ["complete"], "construction-normal") if normal_build == 0 else None
        if normal_build != 0 or normal != 0:
            raise RuntimeError("construction normal control failed")
        try:
            # Expired host cleanup retains the post-launch failed construction.
            HOST.write_text(injected, encoding="utf-8")
            positive_build = run(command, "construction-positive-build")
            positive = run(probe + ["quarantine"], "construction-positive") if positive_build == 0 else None
            # Discard that same finite reservation: the refusal must now fail.
            HOST.write_text(injected.replace("    cleanup_session(session, Clock::now());",
                "    cleanup_session(session, Clock::now());\n    release_reservation(session);"), encoding="utf-8")
            broken_build = run(command, "construction-broken-build")
            broken = run(probe + ["quarantine"], "construction-broken") if broken_build == 0 else None
            HOST.write_text(injected, encoding="utf-8")
            restored_build = run(command, "construction-restored-build")
            restored = run(probe + ["quarantine"], "construction-restored") if restored_build == 0 else None
        finally:
            HOST.write_bytes(original)
            final_build = run(command, "construction-final-build")
            final = run(probe + ["complete"], "construction-final") if final_build == 0 else None
        results["construction-retention"] = dict(source_sha256=hashlib.sha256(original).hexdigest(),
            normal_build=normal_build, normal=normal, positive_build=positive_build, positive=positive,
            broken_build=broken_build, broken=broken, restored_build=restored_build, restored=restored,
            final_build=final_build, final=final)
        (evidence / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
        print("construction-retention", results["construction-retention"], flush=True)
        if positive_build != 0 or positive != 0 or broken_build != 0 or broken in (None, 0) or restored_build != 0 or restored != 0 or final_build != 0 or final != 0:
            raise RuntimeError("failed construction did not preserve quarantine")


if __name__ == "__main__":
    main()
