#!/usr/bin/env python3
import argparse
import json
import re
import sys
from pathlib import Path


REQUIRED_ENTRY_KEYS = {
    "id",
    "renderer",
    "source",
    "width",
    "height",
    "adapter_backend_type",
    "adapter_scope",
    "scene_data_consumed",
    "primitive_count",
    "depth_target_allocated",
    "color_target_allocated",
    "vertex_buffer_uploaded",
    "index_buffer_uploaded",
    "uniform_buffer_uploaded",
    "texture_uploaded",
    "pipeline_cache_entry_count",
    "pipeline_cache_hit_count",
    "command_submitted",
    "readback_completed",
    "pixel_output_produced",
    "min_distinct_color_count",
    "min_non_transparent_pixel_count",
    "fingerprint",
    "cpp_constant",
}

REQUIRED_MANIFEST_KEYS = {
    "schema_version",
    "golden_kind",
    "fingerprint_algorithm",
    "status",
    "entries",
    "software_adapter",
}

REQUIRED_SOFTWARE_ADAPTER_KEYS = {
    "status",
    "required_for_final_phase7",
    "pixel_producing",
    "backend_type",
    "adapter_scope",
    "golden_entry_ids",
    "notes",
}

ALLOWED_MANIFEST_STATUSES = {
    "interim_default_adapter",
    "final_software_adapter",
}

HARDCODED_RENDERER = "Renderer3D::render_hardcoded_textured_cube"
SCENE_DATA_RENDERER = "Renderer3D::render_scene_data"
ALLOWED_RENDERERS = {
    HARDCODED_RENDERER,
    SCENE_DATA_RENDERER,
}


def load_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(f"{path}: invalid JSON: {exc}") from exc


def parse_cpp_constants(path):
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(
        r"inline\s+constexpr\s+uint64_t\s+(\w+)\s*=\s*(\d+)ULL\s*;")
    return {name: value for name, value in pattern.findall(text)}


def require(condition, message, errors):
    if not condition:
        errors.append(message)


def validate_manifest(manifest, cpp_constants):
    errors = []
    require(isinstance(manifest, dict),
            "manifest root must be an object",
            errors)
    if not isinstance(manifest, dict):
        return errors
    require(set(manifest.keys()) == REQUIRED_MANIFEST_KEYS,
            "manifest root keys must be exactly: " +
            ", ".join(sorted(REQUIRED_MANIFEST_KEYS)),
            errors)
    require(manifest.get("schema_version") == 1,
            "schema_version must be 1",
            errors)
    require(manifest.get("golden_kind") == "renderer3d_rgba_fingerprint",
            "golden_kind must be renderer3d_rgba_fingerprint",
            errors)
    require(manifest.get("fingerprint_algorithm") == "fnv1a64",
            "fingerprint_algorithm must be fnv1a64",
            errors)
    status = manifest.get("status")
    require(status in ALLOWED_MANIFEST_STATUSES,
            "status must be one of: " +
            ", ".join(sorted(ALLOWED_MANIFEST_STATUSES)),
            errors)
    entries = manifest.get("entries")
    require(isinstance(entries, list) and len(entries) > 0,
            "entries must be a non-empty array",
            errors)

    seen_ids = set()
    if isinstance(entries, list):
        for index, entry in enumerate(entries):
            prefix = f"entries[{index}]"
            require(isinstance(entry, dict), f"{prefix} must be an object", errors)
            if not isinstance(entry, dict):
                continue
            missing = sorted(REQUIRED_ENTRY_KEYS - set(entry.keys()))
            require(not missing,
                    f"{prefix} missing required keys: {', '.join(missing)}",
                    errors)
            extra = sorted(set(entry.keys()) - REQUIRED_ENTRY_KEYS)
            require(not extra,
                    f"{prefix} has unexpected keys: {', '.join(extra)}",
                    errors)
            entry_id = entry.get("id")
            require(isinstance(entry_id, str) and entry_id,
                    f"{prefix}.id must be a non-empty string",
                    errors)
            require(entry_id not in seen_ids,
                    f"{prefix}.id duplicates {entry_id}",
                    errors)
            seen_ids.add(entry_id)
            require(isinstance(entry.get("width"), int) and entry["width"] > 0,
                    f"{prefix}.width must be a positive integer",
                    errors)
            require(isinstance(entry.get("height"), int) and entry["height"] > 0,
                    f"{prefix}.height must be a positive integer",
                    errors)
            renderer = entry.get("renderer")
            require(renderer in ALLOWED_RENDERERS,
                    f"{prefix}.renderer must be one of: " +
                    ", ".join(sorted(ALLOWED_RENDERERS)),
                    errors)
            require(isinstance(entry.get("source"), str) and entry["source"],
                    f"{prefix}.source must be a non-empty string",
                    errors)
            if status == "interim_default_adapter":
                require(entry.get("adapter_scope") == "macos_default_metal",
                        f"{prefix}.adapter_scope must be macos_default_metal "
                        "for interim_default_adapter manifests",
                        errors)
                require(entry.get("adapter_backend_type") == "Metal",
                        f"{prefix}.adapter_backend_type must be Metal "
                        "for interim_default_adapter manifests",
                        errors)
            require(isinstance(entry.get("scene_data_consumed"), bool),
                    f"{prefix}.scene_data_consumed must be a boolean",
                    errors)
            require(isinstance(entry.get("primitive_count"), int) and
                    entry["primitive_count"] >= 0,
                    f"{prefix}.primitive_count must be a non-negative integer",
                    errors)
            for key in (
                "depth_target_allocated",
                "color_target_allocated",
                "vertex_buffer_uploaded",
                "index_buffer_uploaded",
                "uniform_buffer_uploaded",
                "texture_uploaded",
                "command_submitted",
                "readback_completed",
            ):
                require(isinstance(entry.get(key), bool),
                        f"{prefix}.{key} must be a boolean",
                        errors)
            for key in (
                "pipeline_cache_entry_count",
                "pipeline_cache_hit_count",
            ):
                require(isinstance(entry.get(key), int) and entry[key] >= 0,
                        f"{prefix}.{key} must be a non-negative integer",
                        errors)
            if renderer == HARDCODED_RENDERER:
                require(entry.get("source") == "hardcoded",
                        f"{prefix}.source must be hardcoded for "
                        f"{HARDCODED_RENDERER}",
                        errors)
                require(entry.get("scene_data_consumed") is False,
                        f"{prefix}.scene_data_consumed must be false for "
                        f"{HARDCODED_RENDERER}",
                        errors)
                require(entry.get("primitive_count") == 0,
                        f"{prefix}.primitive_count must be 0 for "
                        f"{HARDCODED_RENDERER}",
                        errors)
                require(entry.get("pipeline_cache_entry_count") == 0,
                        f"{prefix}.pipeline_cache_entry_count must be 0 for "
                        f"{HARDCODED_RENDERER}",
                        errors)
                require(entry.get("pipeline_cache_hit_count") == 0,
                        f"{prefix}.pipeline_cache_hit_count must be 0 for "
                        f"{HARDCODED_RENDERER}",
                        errors)
            if renderer == SCENE_DATA_RENDERER:
                require(entry.get("source") != "hardcoded",
                        f"{prefix}.source must name a scene asset for "
                        f"{SCENE_DATA_RENDERER}",
                        errors)
                require(entry.get("scene_data_consumed") is True,
                        f"{prefix}.scene_data_consumed must be true for "
                        f"{SCENE_DATA_RENDERER}",
                        errors)
                require(isinstance(entry.get("primitive_count"), int) and
                        entry["primitive_count"] > 0,
                        f"{prefix}.primitive_count must be positive for "
                        f"{SCENE_DATA_RENDERER}",
                        errors)
                require(isinstance(entry.get("pipeline_cache_entry_count"), int) and
                        entry["pipeline_cache_entry_count"] > 0,
                        f"{prefix}.pipeline_cache_entry_count must be positive for "
                        f"{SCENE_DATA_RENDERER}",
                        errors)
                primitive_count = entry.get("primitive_count")
                cache_entries = entry.get("pipeline_cache_entry_count")
                cache_hits = entry.get("pipeline_cache_hit_count")
                if all(isinstance(value, int)
                       for value in (primitive_count, cache_entries, cache_hits)):
                    require(cache_entries <= primitive_count,
                            f"{prefix}.pipeline_cache_entry_count must not exceed "
                            "primitive_count",
                            errors)
                    require(cache_entries + cache_hits == primitive_count,
                            f"{prefix}.pipeline_cache_entry_count plus "
                            "pipeline_cache_hit_count must equal primitive_count "
                            f"for {SCENE_DATA_RENDERER}",
                            errors)
            require(isinstance(entry.get("pixel_output_produced"), bool),
                    f"{prefix}.pixel_output_produced must be a boolean",
                    errors)
            require(isinstance(entry.get("min_distinct_color_count"), int) and
                    entry["min_distinct_color_count"] > 0,
                    f"{prefix}.min_distinct_color_count must be a positive integer",
                    errors)
            require(isinstance(entry.get("min_non_transparent_pixel_count"), int) and
                    entry["min_non_transparent_pixel_count"] > 0,
                    f"{prefix}.min_non_transparent_pixel_count must be a positive integer",
                    errors)
            fingerprint = entry.get("fingerprint")
            require(isinstance(fingerprint, str) and fingerprint.isdecimal(),
                    f"{prefix}.fingerprint must be a decimal string",
                    errors)
            constant_name = entry.get("cpp_constant")
            require(isinstance(constant_name, str) and constant_name,
                    f"{prefix}.cpp_constant must be a non-empty string",
                    errors)
            if isinstance(constant_name, str) and constant_name:
                cpp_value = cpp_constants.get(constant_name)
                require(cpp_value is not None,
                        f"{prefix}.cpp_constant {constant_name} not found in C++ test",
                        errors)
                if cpp_value is not None:
                    require(cpp_value == fingerprint,
                            f"{prefix}.fingerprint {fingerprint} does not match "
                            f"{constant_name}={cpp_value}",
                            errors)

    software = manifest.get("software_adapter")
    require(isinstance(software, dict),
            "software_adapter must be an object",
            errors)
    if isinstance(software, dict):
        require(set(software.keys()) == REQUIRED_SOFTWARE_ADAPTER_KEYS,
                "software_adapter keys must be exactly: " +
                ", ".join(sorted(REQUIRED_SOFTWARE_ADAPTER_KEYS)),
                errors)
        require(software.get("required_for_final_phase7") is True,
                "software_adapter.required_for_final_phase7 must be true",
                errors)
        require(isinstance(software.get("status"), str) and software["status"],
                "software_adapter.status must be a non-empty string",
                errors)
        require(isinstance(software.get("notes"), str) and software["notes"],
                "software_adapter.notes must be a non-empty string",
                errors)
        require(isinstance(software.get("pixel_producing"), bool),
                "software_adapter.pixel_producing must be a boolean",
                errors)
        entry_ids = software.get("golden_entry_ids")
        require(isinstance(entry_ids, list),
                "software_adapter.golden_entry_ids must be an array",
                errors)
        if isinstance(entry_ids, list):
            for index, entry_id in enumerate(entry_ids):
                require(isinstance(entry_id, str) and entry_id,
                        f"software_adapter.golden_entry_ids[{index}] "
                        "must be a non-empty string",
                        errors)
        if status == "interim_default_adapter":
            require(software.get("pixel_producing") is False,
                    "interim_default_adapter manifest must not claim a "
                    "pixel-producing software adapter",
                    errors)
            require(software.get("backend_type") is None,
                    "interim_default_adapter software_adapter.backend_type "
                    "must be null",
                    errors)
            require(software.get("adapter_scope") is None,
                    "interim_default_adapter software_adapter.adapter_scope "
                    "must be null",
                    errors)
            require(entry_ids == [],
                    "interim_default_adapter software_adapter.golden_entry_ids "
                    "must be empty",
                    errors)
    return errors


def validate_pixel_software_adapter(manifest):
    errors = []
    entries = manifest.get("entries")
    entries_by_id = {}
    if isinstance(entries, list):
        entries_by_id = {
            entry.get("id"): entry
            for entry in entries
            if isinstance(entry, dict) and isinstance(entry.get("id"), str)
        }

    software = manifest.get("software_adapter")
    if not isinstance(software, dict):
        return ["software_adapter must be an object"]

    require(software.get("pixel_producing") is True,
            "final renderer goldens require a pixel-producing software adapter",
            errors)
    backend_type = software.get("backend_type")
    require(isinstance(backend_type, str) and backend_type,
            "software_adapter.backend_type must name the software backend",
            errors)
    if isinstance(backend_type, str) and backend_type.lower() == "null":
        errors.append("Dawn null backend is not a pixel-producing software adapter")

    adapter_scope = software.get("adapter_scope")
    require(isinstance(adapter_scope, str) and adapter_scope.startswith("software_"),
            "software_adapter.adapter_scope must start with software_",
            errors)

    golden_entry_ids = software.get("golden_entry_ids")
    require(isinstance(golden_entry_ids, list) and len(golden_entry_ids) > 0,
            "software_adapter.golden_entry_ids must list at least one software golden",
            errors)
    if isinstance(golden_entry_ids, list):
        for entry_id in golden_entry_ids:
            if not isinstance(entry_id, str):
                continue
            entry = entries_by_id.get(entry_id)
            require(entry is not None,
                    f"software_adapter.golden_entry_ids references missing entry {entry_id}",
                    errors)
            if entry is not None:
                require(entry.get("adapter_scope") == adapter_scope,
                        f"software golden {entry_id} must use adapter_scope {adapter_scope}",
                        errors)
                require(entry.get("adapter_backend_type") == backend_type,
                        f"software golden {entry_id} must use backend_type {backend_type}",
                        errors)

    return errors


def main():
    parser = argparse.ArgumentParser(
        description="Validate Renderer3D golden manifest metadata.")
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--cpp-test", type=Path, required=True)
    parser.add_argument(
        "--require-pixel-software-adapter",
        action="store_true",
        help="Fail unless the manifest contains final Phase 7 software-adapter goldens.")
    args = parser.parse_args()

    try:
        manifest = load_json(args.manifest)
        cpp_constants = parse_cpp_constants(args.cpp_test)
        errors = validate_manifest(manifest, cpp_constants)
        requires_final_software = (
            args.require_pixel_software_adapter or
            manifest.get("status") == "final_software_adapter"
        )
        if not errors and requires_final_software:
            errors.extend(validate_pixel_software_adapter(manifest))
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

    print(f"{args.manifest}: Renderer3D golden manifest entries="
          f"{len(manifest['entries'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
