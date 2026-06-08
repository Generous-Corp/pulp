#!/usr/bin/env python3
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


ROOT_KEYS = {
    "schema_version",
    "roadmap_item",
    "baseline_kind",
    "base_commit",
    "captured_at_utc",
    "build",
    "hotspots",
    "renderer_golden_gate",
}
BUILD_KEYS = {
    "build_type",
    "configure_command",
    "build_dir",
    "environment",
    "verification",
}
ENVIRONMENT_KEYS = {"host_os", "host_arch", "timing_tool"}
VERIFICATION_KEYS = {"cmake_cache_build_type", "required_cxx_flags"}
HOTSPOT_KEYS = {
    "id",
    "source",
    "target",
    "object_hint",
    "timing_command",
    "sample_wall_seconds",
    "median_wall_seconds",
}
RENDERER_GATE_KEYS = {
    "manifest",
    "validator",
    "cpp_test",
    "ctest_manifest",
    "required_ctests",
    "status_allowed",
    "interim_requires_final_software_adapter",
}
EXPECTED_HOTSPOTS = {
    "widget_bridge_cpp": {
        "source": "core/view/src/widget_bridge.cpp",
        "target": "pulp-view-script",
    },
    "renderer3d_cpp": {
        "source": "core/render/src/renderer3d.cpp",
        "target": "pulp-render",
    },
}
ALLOWED_RENDERER_GOLDEN_STATUSES = {
    "interim_default_adapter",
    "final_software_adapter",
}


def load_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(f"{path}: invalid JSON: {exc}") from exc


def require(condition, message, errors):
    if not condition:
        errors.append(message)


def require_exact_keys(value, expected, label, errors):
    require(isinstance(value, dict), f"{label} must be an object", errors)
    if not isinstance(value, dict):
        return
    require(
        set(value.keys()) == expected,
        f"{label} keys must be exactly: " + ", ".join(sorted(expected)),
        errors,
    )


def repo_path(repo_root, relative_path):
    if not isinstance(relative_path, str) or not relative_path:
        return None
    return repo_root / relative_path


def median(values):
    ordered = sorted(values)
    midpoint = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[midpoint]
    return (ordered[midpoint - 1] + ordered[midpoint]) / 2.0


def validate_build(build, errors):
    require_exact_keys(build, BUILD_KEYS, "build", errors)
    if not isinstance(build, dict):
        return
    require(build.get("build_type") == "Release",
            "build.build_type must be Release",
            errors)
    configure_command = build.get("configure_command")
    require(isinstance(configure_command, str) and configure_command,
            "build.configure_command must be a non-empty string",
            errors)
    if isinstance(configure_command, str):
        for token in (
            "cmake -S . -B",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DPULP_BUILD_EXAMPLES=OFF",
            "-DPULP_ENABLE_SCENE3D=ON",
        ):
            require(token in configure_command,
                    f"build.configure_command must contain {token}",
                    errors)
    require(isinstance(build.get("build_dir"), str) and build["build_dir"],
            "build.build_dir must be a non-empty string",
            errors)

    environment = build.get("environment")
    require_exact_keys(environment, ENVIRONMENT_KEYS, "build.environment", errors)
    if isinstance(environment, dict):
        require(environment.get("timing_tool") == "/usr/bin/time -p",
                "build.environment.timing_tool must be /usr/bin/time -p",
                errors)
        for key in ("host_os", "host_arch"):
            require(isinstance(environment.get(key), str) and environment[key],
                    f"build.environment.{key} must be a non-empty string",
                    errors)

    verification = build.get("verification")
    require_exact_keys(
        verification,
        VERIFICATION_KEYS,
        "build.verification",
        errors,
    )
    if isinstance(verification, dict):
        require(verification.get("cmake_cache_build_type") == "Release",
                "build.verification.cmake_cache_build_type must be Release",
                errors)
        flags = verification.get("required_cxx_flags")
        require(isinstance(flags, list) and len(flags) >= 2,
                "build.verification.required_cxx_flags must list required flags",
                errors)
        if isinstance(flags, list):
            for flag in ("-O3", "-DNDEBUG"):
                require(flag in flags,
                        f"build.verification.required_cxx_flags must include {flag}",
                        errors)


def validate_hotspots(repo_root, build_dir, hotspots, errors):
    require(isinstance(hotspots, list) and len(hotspots) == len(EXPECTED_HOTSPOTS),
            "hotspots must contain exactly widget_bridge_cpp and renderer3d_cpp",
            errors)
    if not isinstance(hotspots, list):
        return

    seen = set()
    for index, hotspot in enumerate(hotspots):
        label = f"hotspots[{index}]"
        require_exact_keys(hotspot, HOTSPOT_KEYS, label, errors)
        if not isinstance(hotspot, dict):
            continue
        hotspot_id = hotspot.get("id")
        require(hotspot_id in EXPECTED_HOTSPOTS,
                f"{label}.id must be one of: " +
                ", ".join(sorted(EXPECTED_HOTSPOTS)),
                errors)
        require(hotspot_id not in seen,
                f"{label}.id duplicates {hotspot_id}",
                errors)
        seen.add(hotspot_id)
        expected = EXPECTED_HOTSPOTS.get(hotspot_id)
        source = hotspot.get("source")
        target = hotspot.get("target")
        if expected is not None:
            require(source == expected["source"],
                    f"{label}.source must be {expected['source']}",
                    errors)
            require(target == expected["target"],
                    f"{label}.target must be {expected['target']}",
                    errors)

        source_path = repo_path(repo_root, source)
        require(source_path is not None and source_path.exists(),
                f"{label}.source file does not exist: {source}",
                errors)
        require(isinstance(target, str) and target,
                f"{label}.target must be a non-empty string",
                errors)

        object_hint = hotspot.get("object_hint")
        require(isinstance(object_hint, str) and object_hint.startswith(build_dir + "/"),
                f"{label}.object_hint must start with {build_dir}/",
                errors)
        timing_command = hotspot.get("timing_command")
        require(isinstance(timing_command, str) and timing_command,
                f"{label}.timing_command must be a non-empty string",
                errors)
        if isinstance(timing_command, str):
            required_fragments = (
                f"touch {source}",
                "CCACHE_DISABLE=1",
                "/usr/bin/time -p",
                f"cmake --build {build_dir}",
                f"--target {target}",
                "-j1",
            )
            for fragment in required_fragments:
                require(fragment in timing_command,
                        f"{label}.timing_command must contain {fragment}",
                        errors)

        samples = hotspot.get("sample_wall_seconds")
        require(isinstance(samples, list) and len(samples) >= 3,
                f"{label}.sample_wall_seconds must contain at least 3 samples",
                errors)
        numeric_samples = []
        if isinstance(samples, list):
            for sample_index, sample in enumerate(samples):
                require(isinstance(sample, (int, float)) and sample > 0,
                        f"{label}.sample_wall_seconds[{sample_index}] "
                        "must be a positive number",
                        errors)
                if isinstance(sample, (int, float)):
                    numeric_samples.append(float(sample))
        recorded_median = hotspot.get("median_wall_seconds")
        require(isinstance(recorded_median, (int, float)) and recorded_median > 0,
                f"{label}.median_wall_seconds must be a positive number",
                errors)
        if (isinstance(recorded_median, (int, float)) and
                len(numeric_samples) == len(samples) and numeric_samples):
            expected_median = median(numeric_samples)
            require(abs(float(recorded_median) - expected_median) < 0.001,
                    f"{label}.median_wall_seconds must equal median "
                    f"{expected_median:.3f}",
                    errors)

    require(seen == set(EXPECTED_HOTSPOTS.keys()),
            "hotspots must contain exactly widget_bridge_cpp and renderer3d_cpp",
            errors)


def validate_renderer_gate(repo_root, gate, errors):
    require_exact_keys(gate, RENDERER_GATE_KEYS, "renderer_golden_gate", errors)
    if not isinstance(gate, dict):
        return 0

    paths = {}
    for key in ("manifest", "validator", "cpp_test", "ctest_manifest"):
        path = repo_path(repo_root, gate.get(key))
        paths[key] = path
        require(path is not None and path.exists(),
                f"renderer_golden_gate.{key} does not exist: {gate.get(key)}",
                errors)

    required_ctests = gate.get("required_ctests")
    require(isinstance(required_ctests, list) and len(required_ctests) > 0,
            "renderer_golden_gate.required_ctests must be a non-empty array",
            errors)
    if isinstance(required_ctests, list) and paths.get("ctest_manifest") is not None:
        ctest_text = paths["ctest_manifest"].read_text(encoding="utf-8")
        for index, ctest_name in enumerate(required_ctests):
            require(isinstance(ctest_name, str) and ctest_name,
                    f"renderer_golden_gate.required_ctests[{index}] "
                    "must be a non-empty string",
                    errors)
            if isinstance(ctest_name, str) and ctest_name:
                require(f"add_test(NAME {ctest_name}" in ctest_text,
                        f"required CTest {ctest_name} not found",
                        errors)

    status_allowed = gate.get("status_allowed")
    require(set(status_allowed or []) == ALLOWED_RENDERER_GOLDEN_STATUSES,
            "renderer_golden_gate.status_allowed must list interim_default_adapter "
            "and final_software_adapter",
            errors)
    require(gate.get("interim_requires_final_software_adapter") is True,
            "renderer_golden_gate.interim_requires_final_software_adapter "
            "must be true",
            errors)

    renderer_entry_count = 0
    if errors:
        return renderer_entry_count

    result = subprocess.run(
        [
            sys.executable,
            str(paths["validator"]),
            str(paths["manifest"]),
            "--cpp-test",
            str(paths["cpp_test"]),
        ],
        cwd=repo_root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        errors.append(
            "renderer_golden_gate.validator failed: " +
            (result.stderr.strip() or result.stdout.strip()))
        return renderer_entry_count

    renderer_manifest = load_json(paths["manifest"])
    status = renderer_manifest.get("status")
    require(status in ALLOWED_RENDERER_GOLDEN_STATUSES,
            "renderer golden manifest status must be interim_default_adapter "
            "or final_software_adapter",
            errors)
    require(renderer_manifest.get("golden_kind") == "renderer3d_rgba_fingerprint",
            "renderer golden manifest golden_kind must be "
            "renderer3d_rgba_fingerprint",
            errors)
    entries = renderer_manifest.get("entries")
    if isinstance(entries, list):
        renderer_entry_count = len(entries)
        require(any(entry.get("pixel_output_produced") is True
                    for entry in entries
                    if isinstance(entry, dict)),
                "renderer golden manifest must include at least one "
                "pixel-producing entry",
                errors)
    else:
        require(False, "renderer golden manifest entries must be an array", errors)

    software_adapter = renderer_manifest.get("software_adapter")
    if status == "interim_default_adapter":
        require(isinstance(software_adapter, dict),
                "interim renderer golden manifest must include software_adapter",
                errors)
        if isinstance(software_adapter, dict):
            require(software_adapter.get("required_for_final_phase7") is True,
                    "interim renderer golden gate must require final software adapter",
                    errors)
            require(software_adapter.get("pixel_producing") is False,
                    "interim renderer golden gate must not claim software pixels",
                    errors)

    return renderer_entry_count


def validate_baseline(manifest, repo_root):
    errors = []
    require_exact_keys(manifest, ROOT_KEYS, "baseline root", errors)
    if not isinstance(manifest, dict):
        return errors, 0, 0
    require(manifest.get("schema_version") == 1,
            "schema_version must be 1",
            errors)
    require(manifest.get("roadmap_item") == "P0.3",
            "roadmap_item must be P0.3",
            errors)
    require(manifest.get("baseline_kind") ==
            "single_translation_unit_rebuild_wall_time",
            "baseline_kind must be single_translation_unit_rebuild_wall_time",
            errors)
    base_commit = manifest.get("base_commit")
    require(isinstance(base_commit, str) and
            re.fullmatch(r"[0-9a-f]{40}", base_commit) is not None,
            "base_commit must be a 40-character lowercase hex commit",
            errors)
    captured_at = manifest.get("captured_at_utc")
    require(isinstance(captured_at, str) and
            re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z",
                         captured_at) is not None,
            "captured_at_utc must be an ISO-8601 UTC timestamp",
            errors)

    build = manifest.get("build")
    validate_build(build, errors)
    build_dir = build.get("build_dir") if isinstance(build, dict) else ""
    validate_hotspots(repo_root, build_dir, manifest.get("hotspots"), errors)
    renderer_entry_count = validate_renderer_gate(
        repo_root,
        manifest.get("renderer_golden_gate"),
        errors)

    hotspot_count = (
        len(manifest.get("hotspots"))
        if isinstance(manifest.get("hotspots"), list)
        else 0
    )
    return errors, hotspot_count, renderer_entry_count


def main():
    parser = argparse.ArgumentParser(
        description="Validate refactor-roadmap build-time and renderer baseline metadata.")
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--repo-root", type=Path)
    args = parser.parse_args()

    repo_root = (
        args.repo_root.resolve()
        if args.repo_root is not None
        else Path(__file__).resolve().parents[2]
    )
    try:
        manifest = load_json(args.manifest)
        errors, hotspot_count, renderer_entry_count = validate_baseline(
            manifest,
            repo_root)
    except OSError as exc:
        print(exc, file=sys.stderr)
        return 2
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 2

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print(
        "refactor_baselines_verified=true "
        f"hotspots={hotspot_count} "
        f"renderer_golden_entries={renderer_entry_count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
