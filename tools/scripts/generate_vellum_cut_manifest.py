#!/usr/bin/env python3
"""Generate the deterministic provenance manifest for Vellum's initial cut.

The selection file names paths at one immutable Pulp commit. Directory entries
are expanded using Git's tree, so the result does not depend on the checkout's
working tree. Every emitted row records the original path, blob identity, mode,
and transfer classification.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tempfile
from typing import Iterable


PINNED_PULP_COMMIT = "2ccff748f0d59da34b01ce1fbceabcf19f452731"
SOURCE_REPOSITORY = "https://github.com/Generous-Corp/pulp"
DEFAULT_SELECTION = Path("docs/contracts/vellum-initial-cut-paths.txt")
DEFAULT_OUTPUT = Path("docs/contracts/vellum-initial-cut-manifest.json")

CLASSIFICATIONS = frozenset(
    {
        "framework-core",
        "authoring-only",
        "platform-adapter",
        "test-only",
        "optional",
        "pulp-specific",
        "unresolved",
        "excluded",
    }
)


class ManifestError(RuntimeError):
    """A selection cannot be represented as a complete cut manifest."""


def _git(repo: Path, *args: str) -> bytes:
    process = subprocess.run(
        ["git", "-C", str(repo), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if process.returncode != 0:
        detail = process.stderr.decode("utf-8", errors="replace").strip()
        raise ManifestError(f"git {' '.join(args)} failed: {detail}")
    return process.stdout


def _validate_path(raw_path: str, *, line_number: int) -> str:
    path = raw_path.strip().rstrip("/")
    parsed = PurePosixPath(path)
    if (
        not path
        or parsed.is_absolute()
        or path.startswith("-")
        or "\\" in path
        or any(part in {"", ".", ".."} for part in parsed.parts)
    ):
        raise ManifestError(
            f"selection line {line_number} has an unsafe repository path: {raw_path!r}"
        )
    return path


def read_selection(path: Path) -> list[tuple[str, str | None]]:
    """Read ``(path, declared classification)`` rows from a selection file.

    Plain path rows use the deterministic classifier below. A tab-separated
    ``classification<TAB>path`` row is available for deliberate exceptions.
    """

    if not path.is_file():
        raise ManifestError(f"selection file does not exist: {path}")

    rows: list[tuple[str, str | None]] = []
    seen: set[str] = set()
    for line_number, raw_line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        stripped = raw_line.strip()
        if not stripped or stripped.startswith("#"):
            continue

        declared: str | None = None
        raw_path = stripped
        if "\t" in raw_line:
            fields = raw_line.split("\t")
            if len(fields) != 2:
                raise ManifestError(
                    f"selection line {line_number} must contain at most one tab"
                )
            declared = fields[0].strip()
            raw_path = fields[1]
            if declared not in CLASSIFICATIONS:
                raise ManifestError(
                    f"selection line {line_number} has unknown classification "
                    f"{declared!r}"
                )

        selected_path = _validate_path(raw_path, line_number=line_number)
        if selected_path in seen:
            raise ManifestError(f"selection contains duplicate path: {selected_path}")
        seen.add(selected_path)
        rows.append((selected_path, declared))

    if not rows:
        raise ManifestError("selection file contains no paths")
    return rows


def derive_classification(path: str) -> tuple[str, str] | None:
    """Return the transfer class and the stable rule name for ``path``."""

    # The first-cut directories are intentionally broader than the portion that
    # can become a public framework package without separation work. Keep these
    # rules more specific than the subsystem fallbacks below: ``unresolved`` is
    # a visible, release-blocking classification, not an implicit core claim.
    if path in {"core/canvas/CMakeLists.txt", "core/render/CMakeLists.txt"}:
        return "unresolved", "pulp-build-graph"

    # These files sit in otherwise reusable subsystems, but their current API
    # or implementation still carries Pulp's audio/plugin product model.  They
    # must be split or generalized before a Vellum transfer; classifying them
    # as framework-core would make the initial cut look cleaner than it is.
    audio_plugin_contaminated_paths = {
        "core/canvas/include/pulp/canvas/font_options.hpp",
        "core/canvas/include/pulp/canvas/font_resolver.hpp",
        "core/canvas/include/pulp/canvas/font_scope.hpp",
        "core/canvas/src/font_resolver.cpp",
        "core/canvas/src/font_scope.cpp",
        "core/render/include/pulp/render/bench/perf_counters.hpp",
        "core/view/include/pulp/view/theme.hpp",
        "core/view/src/theme.cpp",
    }
    if path in audio_plugin_contaminated_paths:
        return "unresolved", "audio-plugin-neutrality-split"

    if path.startswith("core/canvas/"):
        optional_canvas_markers = (
            "/scene/",
            "/platform/mac/cg_canvas",
            "cg_canvas.hpp",
            "image_codecs",
            "lottie_animation",
            "scene.cpp",
            "scene_recording.hpp",
        )
        if any(marker in path for marker in optional_canvas_markers):
            return "optional", "canvas-extension"

    if path.startswith("core/render/"):
        excluded_render_markers = (
            "/platform/android/",
            "metal_surface_ios.mm",
            "render_loop_android.cpp",
            "sdl3_surface",
        )
        if any(marker in path for marker in excluded_render_markers):
            return "excluded", "noninitial-render-platform"
        optional_render_markers = (
            "RENDERER3D_MODULE_MAP.md",
            "atlas_inventory",
            "draco_",
            "gpu_compute",
            "ktx2_",
            "renderer3d",
            "skp_capture",
            "texture_atlas",
        )
        if any(marker in path for marker in optional_render_markers):
            return "optional", "render-extension"

    unresolved_view_paths = {
        "core/view/include/pulp/view/design_import.hpp",
        "core/view/include/pulp/view/design_ir.hpp",
        "core/view/include/pulp/view/design_sources.hpp",
        "core/view/include/pulp/view/view.hpp",
        "core/view/include/pulp/view/view_fwd.hpp",
        "core/view/include/pulp/view/widgets.hpp",
        "core/view/src/design_import.cpp",
        "core/view/src/design_import_internal.hpp",
        "core/view/src/design_import_native_common.cpp",
        "core/view/src/design_import_native_common.hpp",
        "core/view/src/design_ir_analysis.cpp",
        "core/view/src/design_ir_helpers.hpp",
        "core/view/src/design_ir_json.cpp",
        "core/view/src/design_ir_normalize.cpp",
        "core/view/src/view.cpp",
        "core/view/src/widgets.cpp",
        "core/view/src/widgets/visualizers.cpp",
    }
    if (
        path in unresolved_view_paths
        or path.startswith("core/view/include/pulp/view/widgets/")
        or path.startswith("core/view/src/widgets/")
        or path.startswith("core/view/platform/mac/window_host_mac")
    ):
        return "unresolved", "contaminated-view-split"

    if path == "packages/pulp-import-ir/src/types.ts":
        return "unresolved", "pulp-import-ir-types-split"
    if path == "tools/figma-plugin/schema/figma-plugin-export-v1.json":
        return "pulp-specific", "legacy-pulp-figma-schema"

    rules = (
        ("core/view/platform/", "platform-adapter", "core/view/platform"),
        ("packages/pulp-import-ir/", "authoring-only", "pulp-import-ir"),
        ("tools/figma-plugin/", "authoring-only", "figma-schema"),
        ("core/canvas/", "framework-core", "core/canvas"),
        ("core/render/", "framework-core", "core/render"),
        ("core/view/include/", "framework-core", "core/view/include"),
        ("core/view/src/", "framework-core", "core/view/src"),
        ("external/fonts/", "framework-core", "runtime-fonts"),
        ("external/nanosvg/", "framework-core", "nanosvg"),
    )
    for prefix, classification, rule_name in rules:
        if path.startswith(prefix):
            return classification, rule_name

    if path in {"LICENSE.md", "NOTICE.md", "DEPENDENCIES.md"}:
        return "framework-core", "required-legal-metadata"
    return None


def _tree_rows(repo: Path, commit: str, selected_path: str) -> list[dict[str, str]]:
    raw = _git(repo, "ls-tree", "-r", "-z", commit, "--", selected_path)
    rows: list[dict[str, str]] = []
    for record in raw.split(b"\0"):
        if not record:
            continue
        try:
            metadata, encoded_path = record.split(b"\t", maxsplit=1)
            mode, object_type, object_id = metadata.decode("ascii").split(" ")
            source_path = encoded_path.decode("utf-8")
        except (UnicodeDecodeError, ValueError) as error:
            raise ManifestError(
                f"could not parse git tree row for {selected_path!r}"
            ) from error
        if object_type != "blob":
            raise ManifestError(
                f"selected path contains unsupported Git object {object_type}: "
                f"{source_path}"
            )
        rows.append(
            {
                "source_path": source_path,
                "git_blob_sha": object_id,
                "git_mode": mode,
            }
        )
    if not rows:
        raise ManifestError(
            f"selected path is missing or contains no blobs at {commit}: {selected_path}"
        )
    return rows


def generate_manifest(
    *, repo: Path, source_commit: str, selection_path: Path
) -> dict[str, object]:
    repo = repo.resolve()
    selection_on_disk = (
        selection_path if selection_path.is_absolute() else repo / selection_path
    )
    try:
        selection_label = selection_on_disk.resolve().relative_to(repo).as_posix()
    except ValueError as error:
        raise ManifestError(
            f"selection file must be inside the repository: {selection_path}"
        ) from error
    resolved_commit = _git(
        repo, "rev-parse", "--verify", f"{source_commit}^{{commit}}"
    ).decode("ascii").strip()
    selections = read_selection(selection_on_disk)

    entries_by_path: dict[str, dict[str, object]] = {}
    for selected_path, declared in selections:
        for tree_row in _tree_rows(repo, resolved_commit, selected_path):
            source_path = tree_row["source_path"]
            if declared is None:
                derived = derive_classification(source_path)
                if derived is None:
                    raise ManifestError(
                        "selected blob has no classification rule; add a rule or an "
                        f"explicit classification: {source_path}"
                    )
                classification, rule_name = derived
                classification_source = f"derived:{rule_name}"
            else:
                classification = declared
                classification_source = f"declared:{selected_path}"

            existing = entries_by_path.get(source_path)
            if existing is None:
                entries_by_path[source_path] = {
                    **tree_row,
                    "classification": classification,
                    "classification_source": classification_source,
                    "selected_by": [selected_path],
                }
                continue

            if existing["classification"] != classification:
                raise ManifestError(
                    f"overlapping selections classify {source_path} differently: "
                    f"{existing['classification']} vs {classification}"
                )
            selected_by = existing["selected_by"]
            assert isinstance(selected_by, list)
            selected_by.append(selected_path)

    entries = [entries_by_path[path] for path in sorted(entries_by_path)]
    for entry in entries:
        selected_by = entry["selected_by"]
        assert isinstance(selected_by, list)
        selected_by.sort()

    return {
        "schema": "pulp.vellum.initial-cut-manifest.v1",
        "source_repository": SOURCE_REPOSITORY,
        "source_commit": resolved_commit,
        "selection_file": selection_label,
        "entry_count": len(entries),
        "entries": entries,
    }


def serialize_manifest(manifest: dict[str, object]) -> bytes:
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")


def write_atomic(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temporary:
        temporary.write(content)
        temporary.flush()
        os.fsync(temporary.fileno())
        temporary_path = Path(temporary.name)
    os.replace(temporary_path, path)


def _relative_to_repo(path: Path, repo: Path) -> Path:
    absolute = path if path.is_absolute() else repo / path
    try:
        return absolute.resolve().relative_to(repo.resolve())
    except ValueError as error:
        raise ManifestError(f"path must be inside the repository: {path}") from error


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--source-commit", default=PINNED_PULP_COMMIT)
    parser.add_argument("--selection", type=Path, default=DEFAULT_SELECTION)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--verify",
        action="store_true",
        help="fail if the committed output differs from a fresh generation",
    )
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        repo = args.repo.resolve()
        selection_relative = _relative_to_repo(args.selection, repo)
        output_relative = _relative_to_repo(args.output, repo)
        selection = repo / selection_relative
        output = repo / output_relative
        manifest = generate_manifest(
            repo=repo,
            source_commit=args.source_commit,
            selection_path=selection_relative,
        )
        expected = serialize_manifest(manifest)
        if args.verify:
            if not output.is_file():
                raise ManifestError(f"manifest does not exist: {output_relative}")
            if output.read_bytes() != expected:
                raise ManifestError(
                    f"manifest is stale: run {Path(__file__).as_posix()} to regenerate "
                    f"{output_relative}"
                )
            print(f"verified {len(manifest['entries'])} blobs in {output_relative}")
            return 0

        write_atomic(output, expected)
        print(f"wrote {len(manifest['entries'])} blobs to {output_relative}")
        return 0
    except ManifestError as error:
        print(f"vellum-cut-manifest: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
