"""Desktop video compose + design-parity proof commands."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from datetime import datetime, timezone
import json
from pathlib import Path
import shlex


def _safe_path_label(value: str) -> str:
    safe = "".join(ch if ch.isalnum() or ch in {"-", "_", "."} else "-" for ch in value.strip())
    safe = safe.strip("-._")
    return safe or "desktop-proof"


def _parse_context_pairs(items: list[str]) -> dict[str, str]:
    context: dict[str, str] = {}
    for item in items:
        if "=" not in item:
            raise ValueError(f"context must be key=value: {item}")
        key, value = item.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key:
            raise ValueError(f"context key is empty: {item}")
        if value:
            context[key] = value
    return context




def cmd_desktop_compose_video(
    args: argparse.Namespace,
    *,
    compose_desktop_video_proof_fn: Callable[..., dict],
    create_issue_video_variant_fn: Callable[..., dict],
    atomic_write_text_fn: Callable[[Path, str], None],
    print_fn: Callable[[str], None] = print,
) -> int:
    manifest_path = Path(args.manifest).expanduser()
    if not manifest_path.is_file():
        print_fn(f"Error: desktop run manifest not found: {manifest_path}")
        return 1
    manifest_path = manifest_path.resolve()
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print_fn(f"Error: could not read desktop run manifest: {exc}")
        return 1

    run_dir = manifest_path.parent
    video_dir = run_dir / "video"
    output_path = Path(args.output).expanduser().resolve() if args.output else video_dir / "proof-composed.mp4"
    metadata_path = Path(args.metadata).expanduser().resolve() if args.metadata else video_dir / "composed-metadata.json"
    issue_output_path = Path(args.issue_output).expanduser().resolve() if args.issue_output else video_dir / "proof.issue.mp4"
    issue_metadata_path = Path(args.issue_metadata).expanduser().resolve() if args.issue_metadata else video_dir / "issue-metadata.json"
    small_output_path = Path(args.small_output).expanduser().resolve() if getattr(args, "small_output", None) else video_dir / "proof.small.mp4"
    small_metadata_path = Path(args.small_metadata).expanduser().resolve() if getattr(args, "small_metadata", None) else video_dir / "small-metadata.json"
    attachment_budget_bytes = int(float(args.video_attachment_budget_mb) * 1_000_000)
    small_budget_bytes = int(float(getattr(args, "small_video_budget_mb", 10.0)) * 1_000_000)

    template = getattr(args, "template", None) or None
    source_image = Path(args.source_image).expanduser().resolve() if getattr(args, "source_image", None) else None
    source_label = getattr(args, "source_label", None) or None
    diff_image = Path(args.diff_image).expanduser().resolve() if getattr(args, "diff_image", None) else None
    diff_label = getattr(args, "diff_label", None) or None
    title = getattr(args, "title", None) or None
    notes = [note for note in (getattr(args, "note", None) or []) if note]
    if source_image and not source_image.is_file():
        print_fn(f"Error: source image not found: {source_image}")
        return 1
    if diff_image and not diff_image.is_file():
        print_fn(f"Error: diff image not found: {diff_image}")
        return 1

    artifacts = manifest.setdefault("artifacts", {})
    existing_composition = manifest.get("video_proof_composition") if isinstance(manifest.get("video_proof_composition"), dict) else {}
    resolved_template = template or existing_composition.get("template") or "validation-proof"
    artifact_diff_image: Path | None = None
    if diff_image is None and resolved_template == "design-parity":
        artifact_diff = artifacts.get("diff_screenshot")
        if artifact_diff:
            candidate = Path(str(artifact_diff)).expanduser().resolve()
            if candidate.is_file():
                artifact_diff_image = candidate
    diff_image_for_compose = diff_image or artifact_diff_image

    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        metadata_path.parent.mkdir(parents=True, exist_ok=True)
        issue_output_path.parent.mkdir(parents=True, exist_ok=True)
        issue_metadata_path.parent.mkdir(parents=True, exist_ok=True)
        if getattr(args, "small_video", False):
            small_output_path.parent.mkdir(parents=True, exist_ok=True)
            small_metadata_path.parent.mkdir(parents=True, exist_ok=True)
        composed_summary = compose_desktop_video_proof_fn(
            manifest_path,
            output_path,
            template=template,
            source_image=source_image,
            source_label=source_label,
            diff_image=diff_image_for_compose,
            diff_label=diff_label,
            title=title,
            notes=notes,
        )
        atomic_write_text_fn(metadata_path, json.dumps(composed_summary, indent=2) + "\n")
        issue_summary = create_issue_video_variant_fn(
            output_path,
            issue_output_path,
            issue_metadata_path,
            attachment_budget_bytes=attachment_budget_bytes,
        )
        small_summary = None
        if getattr(args, "small_video", False):
            small_summary = create_issue_video_variant_fn(
                output_path,
                small_output_path,
                small_metadata_path,
                attachment_budget_bytes=small_budget_bytes,
            )
    except Exception as exc:
        print_fn(f"Error: {exc}")
        return 1

    manifest["video_composed"] = composed_summary
    manifest["video_issue"] = issue_summary
    if small_summary is not None:
        manifest["video_small"] = small_summary
    if notes:
        manifest["video_proof_notes"] = notes
    if template or source_image or source_label or diff_image_for_compose or diff_label or title or notes:
        composition = manifest.setdefault("video_proof_composition", {})
        if not isinstance(composition, dict):
            composition = {}
            manifest["video_proof_composition"] = composition
        composition.update(
            {
                "template": template or composition.get("template") or "validation-proof",
                "source_image": str(source_image) if source_image else composition.get("source_image"),
                "source_label": source_label if source_label is not None else composition.get("source_label"),
                "diff_image": str(diff_image_for_compose) if diff_image_for_compose else composition.get("diff_image"),
                "diff_label": diff_label if diff_label is not None else composition.get("diff_label"),
                "title": title if title is not None else composition.get("title"),
                "notes": notes if notes else composition.get("notes", []),
            }
        )
    if output_path.exists():
        artifacts["video_composed"] = str(output_path)
    if metadata_path.exists():
        artifacts["video_composed_metadata"] = str(metadata_path)
    if issue_output_path.exists():
        artifacts["video_issue"] = str(issue_output_path)
    if issue_metadata_path.exists():
        artifacts["video_issue_metadata"] = str(issue_metadata_path)
    if small_summary is not None and small_output_path.exists():
        artifacts["video_small"] = str(small_output_path)
    if small_summary is not None and small_metadata_path.exists():
        artifacts["video_small_metadata"] = str(small_metadata_path)
    atomic_write_text_fn(manifest_path, json.dumps(manifest, indent=2) + "\n")

    payload = {
        "manifest": str(manifest_path),
        "video_composed": composed_summary,
        "video_issue": issue_summary,
        "video_small": small_summary,
        "artifacts": {
            "video_composed": artifacts.get("video_composed"),
            "video_composed_metadata": artifacts.get("video_composed_metadata"),
            "video_issue": artifacts.get("video_issue"),
            "video_issue_metadata": artifacts.get("video_issue_metadata"),
            "video_small": artifacts.get("video_small"),
            "video_small_metadata": artifacts.get("video_small_metadata"),
        },
    }
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
    else:
        print_fn("Desktop proof video composed.")
        print_fn(f"  manifest: {manifest_path}")
        print_fn(f"  video_composed: {artifacts.get('video_composed')}")
        print_fn(f"  video_issue: {artifacts.get('video_issue')}")
        print_fn(f"  issue_status: {issue_summary.get('status')}")
        if small_summary is not None:
            print_fn(f"  video_small: {artifacts.get('video_small')}")
            print_fn(f"  small_status: {small_summary.get('status')}")
    return 0


def cmd_desktop_design_diff(
    args: argparse.Namespace,
    *,
    design_parity_diff_summary_fn: Callable[..., dict],
    atomic_write_text_fn: Callable[[Path, str], None],
    print_fn: Callable[[str], None] = print,
) -> int:
    source_image = Path(args.source_image).expanduser()
    if not source_image.is_file():
        print_fn(f"Error: source image not found: {source_image}")
        return 1
    source_image = source_image.resolve()

    native_image: Path | None = Path(args.native_image).expanduser() if getattr(args, "native_image", None) else None
    manifest_path: Path | None = Path(args.manifest).expanduser() if getattr(args, "manifest", None) else None
    if manifest_path:
        if not manifest_path.is_file():
            print_fn(f"Error: desktop run manifest not found: {manifest_path}")
            return 1
        manifest_path = manifest_path.resolve()
        try:
            manifest = json.loads(manifest_path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            print_fn(f"Error: could not read desktop run manifest: {exc}")
            return 1
        if native_image is None:
            screenshot = (manifest.get("artifacts") or {}).get("screenshot")
            if screenshot:
                native_image = Path(str(screenshot)).expanduser()
    if native_image is None:
        print_fn("Error: native image required; pass --native-image or --manifest with artifacts.screenshot")
        return 1
    if not native_image.is_file():
        print_fn(f"Error: native image not found: {native_image}")
        return 1
    native_image = native_image.resolve()

    output = Path(args.output).expanduser().resolve()
    resized_source_output = Path(args.resized_source_output).expanduser().resolve() if getattr(args, "resized_source_output", None) else None
    metadata_path = Path(args.metadata).expanduser().resolve() if getattr(args, "metadata", None) else output.with_suffix(".json")
    try:
        summary = design_parity_diff_summary_fn(
            source_image,
            native_image,
            diff_output_path=output,
            resized_source_output_path=resized_source_output,
            enhance_brightness=float(getattr(args, "enhance_brightness", 3.0)),
        )
    except Exception as exc:
        print_fn(f"Error: {exc}")
        return 1

    payload = {
        "kind": "desktop-design-parity-diff",
        "source_image": str(source_image),
        "native_image": str(native_image),
        "manifest": str(manifest_path) if manifest_path else None,
        "diff_image": str(output),
        "resized_source_image": str(resized_source_output) if resized_source_output else None,
        "metadata": str(metadata_path),
        "summary": summary,
        "compose_args": [
            "--diff-image",
            str(output),
            "--diff-label",
            "Source vs native screenshot diff",
        ],
    }
    atomic_write_text_fn(metadata_path, json.dumps(payload, indent=2) + "\n")
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
        return 0
    print_fn("Desktop design parity diff generated")
    print_fn(f"  source_image: {source_image}")
    print_fn(f"  native_image: {native_image}")
    print_fn(f"  diff_image: {output}")
    if resized_source_output:
        print_fn(f"  resized_source_image: {resized_source_output}")
    print_fn(f"  metadata: {metadata_path}")
    if summary.get("bbox"):
        bbox = summary["bbox"]
        print_fn(f"  bbox: {bbox['left']},{bbox['top']} -> {bbox['right']},{bbox['bottom']}")
    else:
        print_fn("  bbox: unchanged")
    print_fn(f"  compose_args: --diff-image {shlex.quote(str(output))} --diff-label 'Source vs native screenshot diff'")
    return 0


def cmd_desktop_design_proof(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    design_parity_diff_summary_fn: Callable[..., dict],
    compose_desktop_video_proof_fn: Callable[..., dict],
    create_issue_video_variant_fn: Callable[..., dict],
    atomic_write_text_fn: Callable[[Path, str], None],
    now_fn: Callable[[], datetime] | None = None,
    print_fn: Callable[[str], None] = print,
) -> int:
    source_image = Path(args.source_image).expanduser()
    if not source_image.is_file():
        print_fn(f"Error: source image not found: {source_image}")
        return 1
    source_image = source_image.resolve()

    native_image = Path(args.native_image).expanduser()
    if not native_image.is_file():
        print_fn(f"Error: native image not found: {native_image}")
        return 1
    native_image = native_image.resolve()

    label = getattr(args, "label", None) or "design-parity-proof"
    safe_label = _safe_path_label(label)
    now = now_fn() if now_fn else datetime.now(timezone.utc)
    if now.tzinfo is None:
        now = now.replace(tzinfo=timezone.utc)
    completed_at = now.astimezone(timezone.utc).isoformat()
    stamp = now.astimezone(timezone.utc).strftime("%Y%m%d-%H%M%S")

    output_dir_arg = getattr(args, "output_dir", None)
    if output_dir_arg:
        run_dir = Path(output_dir_arg).expanduser().resolve()
    else:
        try:
            config = load_config_fn()
            artifact_root = Path(config["desktop_automation"]["artifact_root"]).expanduser().resolve()
        except Exception as exc:
            print_fn(f"Error: could not resolve desktop artifact root: {exc}")
            return 1
        run_dir = artifact_root / "mac" / "design-proof" / f"{stamp}-{safe_label}"
    video_dir = run_dir / "video"
    manifest_path = run_dir / "manifest.json"
    diff_path = video_dir / "source-vs-native-diff.png"
    diff_metadata_path = video_dir / "source-vs-native-diff.json"
    resized_source_path = video_dir / "source-resized-to-native.png"

    try:
        context = _parse_context_pairs(list(getattr(args, "context", None) or []))
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    notes = [note for note in (getattr(args, "note", None) or []) if note]
    title = getattr(args, "title", None) or "Design parity proof"
    source_label = getattr(args, "source_label", None) or "Source reference"
    diff_label = getattr(args, "diff_label", None) or "Source vs native screenshot diff"

    manifest = {
        "target": "mac",
        "action": "design-parity",
        "label": label,
        "completed_at": completed_at,
        "source": {
            "mode": "still-images",
        },
        "artifacts": {
            "screenshot": str(native_image),
            "video_poster": str(native_image),
        },
        "video_proof_composition": {
            "template": "design-parity",
            "title": title,
            "source_image": str(source_image),
            "source_label": source_label,
            "diff_label": diff_label,
            "context": context,
            "notes": notes,
        },
    }

    try:
        video_dir.mkdir(parents=True, exist_ok=True)
        atomic_write_text_fn(manifest_path, json.dumps(manifest, indent=2) + "\n")
        diff_summary = design_parity_diff_summary_fn(
            source_image,
            native_image,
            diff_output_path=diff_path,
            resized_source_output_path=resized_source_path,
            enhance_brightness=float(getattr(args, "enhance_brightness", 3.0)),
        )
        diff_payload = {
            "kind": "desktop-design-parity-diff",
            "source_image": str(source_image),
            "native_image": str(native_image),
            "manifest": str(manifest_path),
            "diff_image": str(diff_path),
            "resized_source_image": str(resized_source_path),
            "metadata": str(diff_metadata_path),
            "summary": diff_summary,
            "compose_args": [
                "--diff-image",
                str(diff_path),
                "--diff-label",
                diff_label,
            ],
        }
        atomic_write_text_fn(diff_metadata_path, json.dumps(diff_payload, indent=2) + "\n")

        manifest["artifacts"]["diff_screenshot"] = str(diff_path)
        manifest["artifacts"]["diff_metadata"] = str(diff_metadata_path)
        manifest["artifacts"]["source_resized_to_native"] = str(resized_source_path)
        manifest["video_proof_composition"]["diff_image"] = str(diff_path)
        atomic_write_text_fn(manifest_path, json.dumps(manifest, indent=2) + "\n")
    except Exception as exc:
        print_fn(f"Error: {exc}")
        return 1

    compose_lines: list[str] = []
    compose_result = cmd_desktop_compose_video(
        argparse.Namespace(
            manifest=str(manifest_path),
            output=None,
            metadata=None,
            issue_output=None,
            issue_metadata=None,
            small_video=bool(getattr(args, "small_video", False)),
            small_output=None,
            small_metadata=None,
            small_video_budget_mb=float(getattr(args, "small_video_budget_mb", 10.0)),
            template="design-parity",
            source_image=str(source_image),
            source_label=source_label,
            diff_image=str(diff_path),
            diff_label=diff_label,
            title=title,
            note=notes,
            video_attachment_budget_mb=float(getattr(args, "video_attachment_budget_mb", 100.0)),
            json=True,
        ),
        compose_desktop_video_proof_fn=compose_desktop_video_proof_fn,
        create_issue_video_variant_fn=create_issue_video_variant_fn,
        atomic_write_text_fn=atomic_write_text_fn,
        print_fn=compose_lines.append,
    )
    if compose_result != 0:
        for line in compose_lines:
            print_fn(line)
        return compose_result

    compose_payload = json.loads(compose_lines[-1]) if compose_lines else {}
    payload = {
        "kind": "desktop-design-proof",
        "manifest": str(manifest_path),
        "run_dir": str(run_dir),
        "source_image": str(source_image),
        "native_image": str(native_image),
        "diff": diff_payload,
        "video_composed": compose_payload.get("video_composed"),
        "video_issue": compose_payload.get("video_issue"),
        "video_small": compose_payload.get("video_small"),
        "artifacts": compose_payload.get("artifacts", {}),
    }
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
        return 0

    print_fn("Desktop design proof created.")
    print_fn(f"  manifest: {manifest_path}")
    print_fn(f"  source_image: {source_image}")
    print_fn(f"  native_image: {native_image}")
    print_fn(f"  diff_image: {diff_path}")
    if compose_payload.get("artifacts", {}).get("video_composed"):
        print_fn(f"  video_composed: {compose_payload['artifacts']['video_composed']}")
    if compose_payload.get("artifacts", {}).get("video_issue"):
        print_fn(f"  video_issue: {compose_payload['artifacts']['video_issue']}")
    if compose_payload.get("artifacts", {}).get("video_small"):
        print_fn(f"  video_small: {compose_payload['artifacts']['video_small']}")
    return 0


