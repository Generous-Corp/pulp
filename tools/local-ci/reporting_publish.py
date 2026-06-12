"""Desktop automation publish report staging helpers."""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import tempfile
from typing import Callable

from reporting_publish_html import desktop_publish_index_html


ARTIFACT_KEYS = (
    "screenshot",
    "before_screenshot",
    "diff_screenshot",
    "ui_snapshot",
    "stdout",
    "stderr",
)


def clear_directory_contents(path: Path) -> None:
    if not path.exists():
        return
    for child in path.iterdir():
        if child.name == ".git":
            continue
        if child.is_dir():
            shutil.rmtree(child, ignore_errors=True)
        else:
            child.unlink(missing_ok=True)


def copy_directory_contents(src: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for child in src.iterdir():
        target = dest / child.name
        if child.is_dir():
            shutil.copytree(child, target, dirs_exist_ok=True)
        else:
            shutil.copy2(child, target)


def publish_report_to_branch(
    config: dict,
    report: dict,
    *,
    root: Path,
    run_git_fn: Callable[..., object],
    reset_local_worktree_fn: Callable[[Path], None],
    clear_directory_contents_fn: Callable[[Path], None],
    git_origin_http_url_fn: Callable[[Path], str | None],
) -> dict:
    branch = config["desktop_automation"]["publish_branch"]
    report_dir = Path(report["output_dir"]).expanduser()
    report_name = report_dir.name
    publish_root = Path(tempfile.mkdtemp(prefix="pulp-desktop-publish-"))
    worktree = publish_root / "worktree"
    branch_exists = bool(run_git_fn(["ls-remote", "--heads", "origin", branch], cwd=root, check=False).stdout.strip())
    try:
        if branch_exists:
            run_git_fn(["worktree", "add", "--detach", str(worktree), f"origin/{branch}"], cwd=root)
            run_git_fn(["checkout", "-B", branch, f"origin/{branch}"], cwd=worktree)
        else:
            run_git_fn(["worktree", "add", "--detach", str(worktree), "HEAD"], cwd=root)
            run_git_fn(["checkout", "--orphan", branch], cwd=worktree)
            run_git_fn(["rm", "-rf", "--ignore-unmatch", "."], cwd=worktree, check=False)
            clear_directory_contents_fn(worktree)
        dest_root = worktree / "desktop-automation"
        report_dest = dest_root / "reports" / report_name
        latest_dest = dest_root / "latest"
        shutil.rmtree(report_dest, ignore_errors=True)
        shutil.rmtree(latest_dest, ignore_errors=True)
        report_dest.parent.mkdir(parents=True, exist_ok=True)
        latest_dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(report_dir, report_dest)
        shutil.copytree(report_dir, latest_dest)
        run_git_fn(["add", "desktop-automation"], cwd=worktree)
        status = run_git_fn(["status", "--short"], cwd=worktree).stdout.strip()
        if status:
            run_git_fn(["commit", "-m", f"Publish desktop automation report {report_name}"], cwd=worktree)
            run_git_fn(["push", "origin", f"HEAD:{branch}"], cwd=worktree)
        remote_base = git_origin_http_url_fn(root)
        published = {
            "mode": "branch",
            "branch": branch,
            "report_path": f"desktop-automation/reports/{report_name}",
            "latest_path": "desktop-automation/latest",
        }
        if remote_base:
            published["branch_url"] = f"{remote_base}/tree/{branch}"
            published["report_url"] = f"{remote_base}/tree/{branch}/desktop-automation/reports/{report_name}"
            published["latest_url"] = f"{remote_base}/tree/{branch}/desktop-automation/latest"
            published["latest_index_json_url"] = f"{remote_base}/blob/{branch}/desktop-automation/latest/index.json"
            published_runs = []
            for run in report.get("runs", []):
                artifact_urls = {}
                for key, value in (run.get("artifacts") or {}).items():
                    if isinstance(value, str):
                        artifact_urls[key] = f"{remote_base}/blob/{branch}/desktop-automation/latest/{value}"
                published_runs.append(
                    {
                        "label": run.get("label"),
                        "target": run.get("target"),
                        "action": run.get("action"),
                        "artifact_urls": artifact_urls,
                    }
                )
            published["runs"] = published_runs
        return published
    finally:
        reset_local_worktree_fn(worktree)
        shutil.rmtree(publish_root, ignore_errors=True)


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

    report = {
        "generated_at": index_payload["generated_at"],
        "label": index_payload["label"],
        "publish_mode": index_payload["publish_mode"],
        "publish_branch": index_payload["publish_branch"],
        "output_dir": str(publish_dir),
        "index_html": str(index_html),
        "index_json": str(index_json),
        "run_count": len(published_runs),
        "runs": published_runs,
    }
    write_desktop_publish_rollups_fn(config)
    if config["desktop_automation"]["publish_mode"] == "branch":
        report["published"] = publish_report_to_branch_fn(config, report)
    return report
