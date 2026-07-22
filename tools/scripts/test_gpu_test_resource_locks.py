#!/usr/bin/env python3
"""Every test suite that creates a real GPU device must serialize on `pulp_gpu`.

Dawn/Metal device creation and blocking readbacks are a process-global resource.
A suite registered without ``RESOURCE_LOCK pulp_gpu`` runs in parallel with every
other GPU test and contends for it, which surfaces as an intermittent short
readback — the frame composites fewer pixels than the assertion expects — on a PR
that changed nothing related. The render/GPU and gpu-audio manifests already take
this lock; this test keeps a newly added suite from silently skipping it, because
the symptom appears on somebody else's unrelated PR days later.

Run:  python3 tools/scripts/test_gpu_test_resource_locks.py
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
TEST_DIR = REPO_ROOT / "test"
MANIFEST_DIR = TEST_DIR / "cmake"

# Markers that mean the suite stands up a real Dawn/Graphite device and reads
# pixels back, as opposed to exercising GPU-adjacent logic on the CPU.
DEVICE_MARKERS = (
    "make_offscreen_fixture",
    "render_gpu_frame",
    "GraphiteContext",
    "graphite_context",
    "GpuSurface",
)

# Deliberately NOT a marker: the bare word "Dawn". It appears in prose — one
# suite's header comment says the logic under test "is Dawn-free" — so matching
# it flags CPU-only suites. Only concrete API symbols are reliable here.

# GPU-named suites that deliberately stay parallel because they never create a
# device. The manifests say so directly: "The GPU-agnostic transport/flow-pans
# suites above do NOT create devices and are intentionally left parallel."
INTENTIONALLY_PARALLEL = {
    "pulp-test-gpu-audio-transport",
    "pulp-test-flow-pans",
}

# Sources that name a GPU type without standing up a device: they SUBCLASS the
# interface to supply a mock. Referencing `GpuSurface` is not the same as
# creating one, and a mock costs no GPU, so these must stay parallel.
MOCK_ONLY_SOURCES = {
    "test_scripted_ui.cpp",  # class TestGpuSurface : public GpuSurface, backend_type "Mock"
}

LOCK = "RESOURCE_LOCK pulp_gpu"
# The render/GPU manifest passes the lock through this variable rather than
# spelling it out at each call site.
LOCK_VIA_VARIABLE = "PULP_GPU_TEST_DISCOVERY_ARGS"


def manifests() -> list[Path]:
    return sorted(MANIFEST_DIR.glob("*.cmake"))


def gpu_device_sources() -> set[str]:
    """Test sources that create a real GPU device."""
    found = set()
    for path in sorted(TEST_DIR.glob("test_*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        if path.name in MOCK_ONLY_SOURCES:
            continue
        if any(marker in text for marker in DEVICE_MARKERS):
            found.add(path.name)
    return found


def _calls(text: str, names: tuple[str, ...]) -> list[tuple[str, str]]:
    """Yield (target, body) for each `name(target ...)` call, paren-balanced.

    A regex cannot do this: these calls are sometimes one line
    (`add_executable(t src.cpp)`) and sometimes many, and the bodies contain
    nested `$<...>` generator expressions and parenthesised conditions.
    """
    out = []
    for name in names:
        idx = 0
        while True:
            hit = text.find(name + "(", idx)
            if hit == -1:
                break
            start = hit + len(name) + 1
            depth, i = 1, start
            while i < len(text) and depth:
                if text[i] == "(":
                    depth += 1
                elif text[i] == ")":
                    depth -= 1
                i += 1
            body = text[start:i - 1]
            target = body.strip().split()[0] if body.strip() else ""
            if target:
                out.append((target, body))
            idx = i
    return out


def registrations() -> dict[str, tuple[str, Path]]:
    """Map each test target to (declaration_body, manifest) declaring its sources."""
    out: dict[str, tuple[str, Path]] = {}
    for manifest in manifests():
        text = manifest.read_text(encoding="utf-8")
        for target, body in _calls(text, ("add_executable", "pulp_add_test_suite")):
            out.setdefault(target, (body, manifest))
    return out


def lock_text_for(target: str, manifest: Path) -> str:
    """All manifest text that could carry the lock for this target."""
    text = manifest.read_text(encoding="utf-8")
    chunks = [
        body
        for name, body in _calls(
            text,
            (
                "catch_discover_tests",
                "pulp_add_test_suite",
                "set_tests_properties",
                "add_executable",
            ),
        )
        if name == target
    ]
    return "\n".join(chunks)


class GpuResourceLockTests(unittest.TestCase):
    def test_device_creating_suites_take_the_gpu_lock(self) -> None:
        sources = gpu_device_sources()
        self.assertTrue(sources, "no GPU-device test sources found — detector broke")

        regs = registrations()
        checked = 0
        for target, (body, manifest) in regs.items():
            if target in INTENTIONALLY_PARALLEL:
                continue
            used = {s for s in sources if s in body}
            if not used:
                continue
            checked += 1
            with self.subTest(target=target, manifest=manifest.name):
                text = lock_text_for(target, manifest)
                self.assertTrue(
                    LOCK in text or LOCK_VIA_VARIABLE in text,
                    f"{target} ({manifest.name}) builds {sorted(used)}, which creates a "
                    f"real GPU device, but is registered without {LOCK!r}. It will "
                    f"contend with every other GPU test under parallel ctest.",
                )
        self.assertGreater(checked, 0, "matched no GPU suites — mapping broke")

    def test_known_offscreen_suites_are_covered(self) -> None:
        """Guard the detector itself, not just the policy.

        If a rename or refactor stops these from being recognised as
        device-creating, the policy test above would silently pass by checking
        nothing at all.
        """
        sources = gpu_device_sources()
        for name in (
            "test_subtree_cache_gpu.cpp",
            "test_partial_repaint_gpu.cpp",
            "test_plugin_editor_headless_gpu.cpp",
            "test_font_rendering_goldens_gpu.cpp",
        ):
            with self.subTest(source=name):
                self.assertIn(name, sources)


if __name__ == "__main__":
    unittest.main(verbosity=2)
