#!/usr/bin/env python3
"""Self-test for intel_canary_lint.py.

Runs a synthetic true/false-positive corpus through each detector plus an
end-to-end pass that asserts the real tree is clean. Pytest-compatible
(functions named test_*) and also runnable directly:

    python3 tools/scripts/test_intel_canary_lint.py
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import intel_canary_lint as lint  # noqa: E402

_EMPTY = lint.Allowlist([])


def _classes(findings) -> list[int]:
    return sorted(f.klass for f in findings)


# ── Class 1: NEON outside a guard ────────────────────────────────────────────
def test_class1_unguarded_neon_flagged():
    text = (
        "#include <arm_neon.h>\n"
        "void f(const float* a, float* b) {\n"
        "    float32x4_t v = vld1q_f32(a);\n"
        "    vst1q_f32(b, vaddq_f32(v, v));\n"
        "}\n"
    )
    f = lint.find_neon_and_arm_simd("core/signal/src/x.cpp", text)
    assert 1 in _classes(f), "unguarded NEON include + intrinsic must flag C1"


def test_class1_guarded_neon_clean():
    text = (
        "#if defined(__aarch64__)\n"
        "#include <arm_neon.h>\n"
        "static float32x4_t g(const float* a) { return vld1q_f32(a); }\n"
        "#endif\n"
    )
    f = lint.find_neon_and_arm_simd("core/signal/src/x.cpp", text)
    assert 1 not in _classes(f), "NEON under __aarch64__ must be clean"


def test_class1_vst3_is_not_neon():
    # Regression: the VST3 plugin format (and vst1/vst2 variants) must never be
    # mistaken for the vst1..vst4 NEON store intrinsics.
    text = (
        '#include <pulp/format/vst3_adapter.hpp>\n'
        'namespace pulp::format::vst3 {\n'
        'const char* fmt() { return "vst3"; }  // vst3q lookalike in a comment\n'
        "}\n"
    )
    f = lint.find_neon_and_arm_simd("core/format/src/vst3_adapter.cpp", text)
    assert 1 not in _classes(f), "vst3 tokens must not be read as NEON"


# ── Class 2: ARM-gated SIMD with no x86 sibling / no #else ────────────────────
def test_class2_arm_only_simd_flagged():
    text = (
        "#if defined(__aarch64__)\n"
        "    acc = vfmaq_f32(acc, x, y);\n"
        "#endif\n"
    )
    f = lint.find_neon_and_arm_simd("core/signal/src/s.cpp", text)
    assert 2 in _classes(f), "ARM-only SIMD with no x86 branch/#else must flag C2"


def test_class2_with_x86_sibling_clean():
    text = (
        "#if defined(__x86_64__)\n"
        "    acc = _mm_add_ps(acc, x);\n"
        "#elif defined(__aarch64__)\n"
        "    acc = vaddq_f32(acc, x);\n"
        "#endif\n"
    )
    f = lint.find_neon_and_arm_simd("core/signal/src/s.cpp", text)
    assert 2 not in _classes(f), "x86 sibling branch means no dropped fallback"


def test_class2_with_else_fallback_clean():
    text = (
        "#if defined(__aarch64__)\n"
        "    acc = vmulq_f32(a, b);\n"
        "#else\n"
        "    acc = a * b;  // portable scalar fallback\n"
        "#endif\n"
    )
    f = lint.find_neon_and_arm_simd("core/signal/src/s.cpp", text)
    assert 2 not in _classes(f), "#else scalar fallback covers x86"


def test_class2_no_simd_tokens_clean():
    text = (
        "#if defined(__aarch64__)\n"
        '    arch = "arm64";\n'
        "#endif\n"
    )
    f = lint.find_neon_and_arm_simd("core/runtime/src/i.cpp", text)
    assert 2 not in _classes(f), "arch label with no SIMD is not a SIMD chain"


# ── Class 3: hardcoded arm asset/arch string ─────────────────────────────────
def test_class3_hardcoded_slug_flagged():
    text = 'set(_pulp_asset "darwin-arm64")\n'
    f = lint.find_arch_strings("tools/cmake/PulpNew.cmake", text, _EMPTY)
    assert 3 in _classes(f), "a fresh hardcoded darwin-arm64 slug must flag C3"


def test_class3_allowlisted_clean():
    text = 'set(_pulp_asset "darwin-arm64")\n'
    allow = lint.Allowlist(
        [("tools/cmake/PulpNew.cmake", 'set(_pulp_asset "darwin-arm64")')]
    )
    f = lint.find_arch_strings("tools/cmake/PulpNew.cmake", text, allow)
    assert 3 not in _classes(f), "allowlist substring must exempt the line"


def test_class3_comment_skipped():
    text = "# note: darwin-arm64 was the old-only asset\n"
    f = lint.find_arch_strings("tools/cmake/PulpNew.cmake", text, _EMPTY)
    assert 3 not in _classes(f), "comment lines are not build logic"


# ── Class 4: CMAKE_SYSTEM_PROCESSOR for an Apple decision ─────────────────────
def test_class4_apple_host_arch_flagged():
    text = (
        "if(APPLE)\n"
        '  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")\n'
        '    set(SLICE "mac")\n'
        "  endif()\n"
        "endif()\n"
    )
    f = lint.find_system_processor_apple("tools/cmake/PulpNew.cmake", text, _EMPTY)
    assert 4 in _classes(f), "host arch driving an Apple decision must flag C4"


def test_class4_consulting_osx_arch_clean():
    text = (
        "if(APPLE)\n"
        "  if(CMAKE_OSX_ARCHITECTURES MATCHES arm64)\n"
        '    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" host)\n'
        "  endif()\n"
        "endif()\n"
    )
    f = lint.find_system_processor_apple("tools/cmake/PulpNew.cmake", text, _EMPTY)
    assert 4 not in _classes(f), "file consulting CMAKE_OSX_ARCHITECTURES is clean"


def test_class4_non_apple_clean():
    text = 'if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")\n  set(X 1)\nendif()\n'
    f = lint.find_system_processor_apple("tools/cmake/PulpLinux.cmake", text, _EMPTY)
    assert 4 not in _classes(f), "no Apple context, no C4"


# ── Class 5: hardcoded CMAKE_OSX_ARCHITECTURES=arm64 ─────────────────────────
def test_class5_arm64_only_set_flagged():
    text = "set(CMAKE_OSX_ARCHITECTURES arm64)\n"
    f = lint.find_hardcoded_osx_arch("tools/cmake/PulpNew.cmake", text, _EMPTY)
    assert 5 in _classes(f), "arm64-only CMAKE_OSX_ARCHITECTURES must flag C5"


def test_class5_universal_clean():
    text = 'set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")\n'
    f = lint.find_hardcoded_osx_arch("tools/cmake/PulpNew.cmake", text, _EMPTY)
    assert 5 not in _classes(f), "universal value names x86_64, not arm64-only"


def test_class5_read_is_not_a_set():
    text = 'if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")\n  set(X 1)\nendif()\n'
    f = lint.find_hardcoded_osx_arch("tools/cmake/PulpNew.cmake", text, _EMPTY)
    assert 5 not in _classes(f), "a MATCHES read must not be flagged as a set"


def test_class5_dflag_arm64_only_flagged():
    text = "COMMAND cmake -DCMAKE_OSX_ARCHITECTURES=arm64 -B build\n"
    f = lint.find_hardcoded_osx_arch("tools/cmake/PulpNew.cmake", text, _EMPTY)
    assert 5 in _classes(f), "-D...=arm64 (no x86_64) must flag C5"


# ── End-to-end: the real tree must be clean ──────────────────────────────────
def test_tree_is_clean():
    root = lint._repo_root()
    allowlist = root / "tools" / "scripts" / "intel_canary_allowlist.txt"
    findings = lint.run("tree", "origin/main", root, allowlist)
    assert findings == [], (
        "intel_canary_lint must be clean on the current tree; a finding here is "
        "a real cross-arch regression, not a lint bug:\n"
        + "\n".join(f.render() for f in findings)
    )


def _run_all() -> int:
    funcs = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for fn in funcs:
        try:
            fn()
            print(f"ok   {fn.__name__}")
        except AssertionError as exc:
            failed += 1
            print(f"FAIL {fn.__name__}: {exc}")
    print(f"\n{len(funcs) - failed}/{len(funcs)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
