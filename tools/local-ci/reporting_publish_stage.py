"""Desktop automation publish report staging."""

from __future__ import annotations

import json
from pathlib import Path
import shutil
from typing import Callable

from reporting_publish_html import desktop_publish_index_html


ARTIFACT_KEYS = (
    "screenshot",
    "before_screenshot",
    "diff_screenshot",
    "ui_snapshot",
    "video",
    "video_composed",
    "video_issue",
    "video_small",
    "video_focus",
    "video_metadata",
    "video_composed_metadata",
    "video_issue_metadata",
    "video_small_metadata",
    "video_poster",
    "stdout",
    "stderr",
)


def slugify_token(value: str, *, max_len: int = 48) -> str:
    cleaned = "".join(ch.lower() if ch.isalnum() else "-" for ch in value.strip())
    while "--" in cleaned:
        cleaned = cleaned.replace("--", "-")
    cleaned = cleaned.strip("-")
    if not cleaned:
        return "run"
    return cleaned[:max_len]


def stage_desktop_publish_report(
    config: dict,
    manifests: list[dict],
    *,
    output_dir: Path | None = None,
    label: str | None = None,
    create_desktop_publish_bundle_fn: Callable[[dict], Path],
    now_iso_fn: Callable[[], str],
    atomic_write_text_fn: Callable[[Path, str], None],
    write_desktop_publish_rollups_fn: Callable[[dict], None],
    publish_report_to_branch_fn: Callable[[dict, dict], dict],
) -> dict:
    if not manifests:
        raise ValueError("Desktop publish requires at least one run manifest.")

    # Lazy import avoids a reporting_publish <-> reporting_video import cycle.
    from reporting_video import _proof_notes_from_manifest

    publish_dir = output_dir.expanduser() if output_dir else create_desktop_publish_bundle_fn(config)
    publish_dir.mkdir(parents=True, exist_ok=True)
    assets_root = publish_dir / "assets"
    assets_root.mkdir(parents=True, exist_ok=True)

    published_runs: list[dict] = []
    for index, manifest in enumerate(manifests, start=1):
        run_slug = "-".join(
            [
                f"run-{index:02d}",
                slugify_token(str(manifest.get("target", "target"))),
                slugify_token(str(manifest.get("action", "run"))),
                slugify_token(str(manifest.get("label", "artifact"))),
            ]
        )
        run_dir = assets_root / run_slug
        run_dir.mkdir(parents=True, exist_ok=True)

        copied_artifacts: dict[str, str | dict | None] = {}
        for key in ARTIFACT_KEYS:
            path_str = manifest.get("artifacts", {}).get(key)
            if not path_str:
                continue
            source = Path(path_str).expanduser()
            if not source.exists():
                continue
            destination = run_dir / source.name
            shutil.copy2(source, destination)
            copied_artifacts[key] = str(destination.relative_to(publish_dir))

        bundle_dir = Path(manifest.get("artifacts", {}).get("bundle_dir", "")).expanduser()
        manifest_path = bundle_dir / "manifest.json"
        if manifest_path.exists():
            destination = run_dir / "manifest.json"
            shutil.copy2(manifest_path, destination)
            copied_artifacts["manifest"] = str(destination.relative_to(publish_dir))

        if manifest.get("artifacts", {}).get("image_change"):
            copied_artifacts["image_change"] = manifest["artifacts"]["image_change"]

        published_runs.append(
            {
                "target": manifest.get("target"),
                "action": manifest.get("action"),
                "label": manifest.get("label"),
                "completed_at": manifest.get("completed_at"),
                "bundle_dir": manifest.get("artifacts", {}).get("bundle_dir"),
                "interaction_mode": (manifest.get("interaction") or {}).get("mode"),
                "video_proof_composition": manifest.get("video_proof_composition"),
                "video_proof_notes": _proof_notes_from_manifest(manifest),
                "artifacts": copied_artifacts,
            }
        )

    index_payload = {
        "generated_at": now_iso_fn(),
        "label": label or "desktop-publish",
        "publish_mode": config["desktop_automation"]["publish_mode"],
        "publish_branch": config["desktop_automation"]["publish_branch"],
        "run_count": len(published_runs),
        "runs": published_runs,
    }

    index_json = publish_dir / "index.json"
    atomic_write_text_fn(index_json, json.dumps(index_payload, indent=2) + "\n")

    index_html = publish_dir / "index.html"
    atomic_write_text_fn(index_html, desktop_publish_index_html(index_payload))

    # Video-proof review loop: emit the human review markdown + the
    # review-package.json that `desktop review-issue` consumes. Imported lazily
    # to avoid a reporting_publish <-> reporting_video import cycle.
    from reporting_review import desktop_review_issue_body, desktop_review_package

    review_markdown = publish_dir / "review.md"
    atomic_write_text_fn(review_markdown, desktop_review_issue_body(index_payload, publish_dir=publish_dir))
    review_package = publish_dir / "review-package.json"
    review_package_payload = desktop_review_package(index_payload, publish_dir=publish_dir)
    atomic_write_text_fn(review_package, json.dumps(review_package_payload, indent=2) + "\n")

    report = {
        "generated_at": index_payload["generated_at"],
        "label": index_payload["label"],
        "publish_mode": index_payload["publish_mode"],
        "publish_branch": index_payload["publish_branch"],
        "output_dir": str(publish_dir),
        "index_html": str(index_html),
        "index_json": str(index_json),
        "review_markdown": str(review_markdown),
        "review_package": str(review_package),
        "run_count": len(published_runs),
        "runs": published_runs,
    }
    write_desktop_publish_rollups_fn(config)
    if config["desktop_automation"]["publish_mode"] == "branch":
        report["published"] = publish_report_to_branch_fn(config, report)
    return report


__all__ = ["ARTIFACT_KEYS", "slugify_token", "stage_desktop_publish_report"]
