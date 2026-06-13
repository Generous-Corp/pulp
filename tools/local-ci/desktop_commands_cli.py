"""Desktop automation management command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable, Mapping
from functools import partial
import http.server
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess


def _print_lines(lines, *, print_fn: Callable[[str], None]) -> None:
    for line in lines:
        print_fn(line)


def serve_directory(path: Path, *, host: str, port: int) -> None:
    handler = partial(http.server.SimpleHTTPRequestHandler, directory=str(path))
    with http.server.ThreadingHTTPServer((host, port), handler) as server:
        server.serve_forever()


def _path_is_relative_to(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def desktop_publish_root_from_config(config: dict) -> Path:
    return Path(config["desktop_automation"]["artifact_root"]).expanduser().resolve() / "_published"


def _append_unique(items: list[str], value: str | None) -> None:
    value = (value or "").strip()
    if value and value not in items:
        items.append(value)


def desktop_serve_candidate_hosts(
    bind_host: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    which_fn: Callable[[str], str | None] = shutil.which,
    environ: Mapping[str, str] = os.environ,
    hostname_fn: Callable[[], str] = socket.gethostname,
) -> list[str]:
    hosts: list[str] = []
    if bind_host in {"0.0.0.0", "::"}:
        _append_unique(hosts, "127.0.0.1")
        hostname = hostname_fn()
        _append_unique(hosts, hostname)
        if hostname and "." not in hostname:
            _append_unique(hosts, f"{hostname}.local")
        explicit_hosts = environ.get("PULP_DESKTOP_SERVE_HOSTS") or environ.get("PULP_DESKTOP_SERVE_PUBLIC_HOSTS")
        if explicit_hosts:
            for item in explicit_hosts.split(","):
                _append_unique(hosts, item)
        if which_fn("tailscale"):
            try:
                result = run_fn(["tailscale", "ip", "-4"], capture_output=True, text=True, timeout=3, check=False)
                if result.returncode == 0:
                    for line in (result.stdout or "").splitlines():
                        _append_unique(hosts, line)
            except (OSError, subprocess.SubprocessError):
                pass
    else:
        _append_unique(hosts, bind_host)
    return hosts


def desktop_serve_candidate_urls(bind_host: str, port: int, **kwargs) -> list[str]:
    return [f"http://{host}:{port}/" for host in desktop_serve_candidate_hosts(bind_host, **kwargs)]


def cmd_desktop_status(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_receipt_for_fn: Callable[[str], dict],
    desktop_capabilities_for_fn: Callable[[str, str, dict | None], list[str]],
    desktop_optional_capabilities_fn: Callable[[dict | None], list[str]],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    desktop_run_summary_fn: Callable[[dict, dict], dict],
    desktop_proof_summaries_fn: Callable[..., list[dict]],
    normalize_desktop_optional_config_fn: Callable[[dict | None], dict],
    desktop_target_contract_fn: Callable[[str, dict], dict],
    desktop_publish_reports_fn: Callable[..., list[dict]],
    desktop_status_lines_fn: Callable[..., list[str]],
    short_sha_fn: Callable[[str], str],
    windows_tooling_detail_fn: Callable[..., str],
    windows_repo_checkout_detail_fn: Callable[..., str],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    desktop_cfg = config["desktop_automation"]
    targets = desktop_cfg.get("targets", {})
    if args.target:
        if args.target not in targets:
            print_fn(f"\nError: unknown desktop target `{args.target}`")
            return 1
        target_names = [args.target]
    else:
        target_names = sorted(targets)

    target_payloads: list[dict] = []
    for name in target_names:
        target = targets[name]
        receipt = desktop_receipt_for_fn(name)
        capabilities = ", ".join(
            desktop_capabilities_for_fn(target["adapter"], target["capability_tier"], target.get("optional"))
        )
        optional_capabilities = desktop_optional_capabilities_fn(target.get("optional"))
        latest = desktop_run_manifests_fn(config, target_name=name)[:1]
        latest_manifest = latest[0] if latest else None
        latest_run = desktop_run_summary_fn(config, latest_manifest) if latest_manifest else None
        latest_proof_matches = desktop_proof_summaries_fn(config, target_name=name, limit=1)
        latest_proof = latest_proof_matches[0] if latest_proof_matches else None
        target_info = {
            "name": name,
            "enabled": target["enabled"],
            "adapter": target["adapter"],
            "bootstrap": target["bootstrap"],
            "type": target["target_type"],
            "host": target.get("host"),
            "repo_path": target.get("repo_path"),
            "capability_tier": target["capability_tier"],
            "capabilities": desktop_capabilities_for_fn(target["adapter"], target["capability_tier"], target.get("optional")),
            "capabilities_text": capabilities,
            "optional_features": normalize_desktop_optional_config_fn(target.get("optional")),
            "optional_capabilities": optional_capabilities,
            "installed": bool(receipt),
            "installed_at": receipt.get("installed_at", "?") if receipt else None,
            "contract": receipt.get("contract") if receipt else desktop_target_contract_fn(name, target),
            "remote_bootstrap_ready": receipt.get("remote_bootstrap_ready") if receipt else None,
            "remote_tooling_ready": receipt.get("remote_tooling_ready") if receipt else None,
            "remote_repo_checkout_ready": receipt.get("remote_repo_checkout_ready") if receipt else None,
            "tooling_probe": receipt.get("tooling_probe") if receipt else None,
            "repo_checkout_probe": receipt.get("repo_checkout_probe") if receipt else None,
            "latest_run": None,
            "latest_proof": latest_proof,
        }
        if latest_run:
            target_info["latest_run"] = {
                "label": latest_run["label"],
                "completed_at": latest_run["completed_at"],
                "interaction_mode": latest_run["interaction_mode"],
                "run_status": latest_run["run_status"],
                "source_mode": latest_run["source"]["mode"],
                "source_branch": latest_run["source"]["branch"],
                "source_sha": latest_run["source"]["sha"],
                "proof_scope": latest_run["proof_scope"],
                "host": latest_run["host"],
                "screenshot": latest_run["artifacts"]["screenshot"],
                "before_screenshot": latest_run["artifacts"]["before_screenshot"],
                "diff_screenshot": latest_run["artifacts"]["diff_screenshot"],
                "image_change": latest_run["artifacts"]["image_change"],
                "ui_snapshot": latest_run["artifacts"]["ui_snapshot"],
                "video": latest_run["artifacts"].get("video"),
                "video_composed": latest_run["artifacts"].get("video_composed"),
                "bundle_dir": latest_run["artifacts"]["bundle_dir"],
            }
        target_payloads.append(target_info)

    latest_publish_matches = desktop_publish_reports_fn(config, limit=1)
    latest_publish = latest_publish_matches[0] if latest_publish_matches else None
    if getattr(args, "json", False):
        payload = {
            "artifact_root": desktop_cfg["artifact_root"],
            "publish_mode": desktop_cfg["publish_mode"],
            "publish_branch": desktop_cfg["publish_branch"],
            "retention_days": desktop_cfg["retention_days"],
            "latest_publish": latest_publish,
            "targets": target_payloads,
        }
        print_fn(json.dumps(payload, indent=2))
        return 0

    _print_lines(
        desktop_status_lines_fn(
            desktop_cfg,
            target_payloads,
            latest_publish=latest_publish,
            short_sha_fn=short_sha_fn,
            windows_tooling_detail_fn=windows_tooling_detail_fn,
            windows_repo_checkout_detail_fn=windows_repo_checkout_detail_fn,
        ),
        print_fn=print_fn,
    )
    return 0


def cmd_desktop_config_show(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_config_show_lines_fn: Callable[[dict], list[str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    desktop_cfg = config["desktop_automation"]
    if getattr(args, "json", False):
        print_fn(json.dumps(desktop_cfg, indent=2))
        return 0

    _print_lines(desktop_config_show_lines_fn(desktop_cfg), print_fn=print_fn)
    return 0


def cmd_desktop_config_set(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    save_config_fn: Callable[[dict], None],
    config_path_fn: Callable[[], Path],
    normalize_publish_mode_fn: Callable[[str], str],
    parse_config_bool_fn: Callable[[str], bool],
    normalize_desktop_config_fn: Callable[[dict], dict],
    desktop_config_update_lines_fn: Callable[[dict], list[str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    desktop_cfg = config.setdefault("desktop_automation", {})
    key = args.key
    raw_value = args.value
    payload_value = None
    try:
        if key == "artifact_root":
            desktop_cfg["artifact_root"] = raw_value
            payload_value = desktop_cfg["artifact_root"]
        elif key == "publish_mode":
            desktop_cfg["publish_mode"] = normalize_publish_mode_fn(raw_value)
            payload_value = desktop_cfg["publish_mode"]
        elif key == "publish_branch":
            desktop_cfg["publish_branch"] = raw_value
            payload_value = desktop_cfg["publish_branch"]
        elif key == "retention_days":
            retention_days = int(raw_value)
            if retention_days < 0:
                raise ValueError("retention_days must be >= 0")
            desktop_cfg["retention_days"] = retention_days
            payload_value = desktop_cfg["retention_days"]
        elif key.startswith("target."):
            parts = key.split(".")
            if len(parts) != 3:
                raise ValueError("Target desktop config keys must look like target.<name>.<field>.")
            _, target_name, field = parts
            target_cfg = desktop_cfg.setdefault("targets", {}).setdefault(target_name, {})
            optional_cfg = dict(target_cfg.get("optional", {}))
            if field in {"webview_driver", "debug_attach", "video_capture", "frame_stats"}:
                optional_cfg[field] = parse_config_bool_fn(raw_value)
            elif field in {"webdriver_url", "debugger_command"}:
                optional_cfg[field] = raw_value
            else:
                raise ValueError(
                    "Unsupported target desktop config field. Use one of: "
                    "target.<name>.webview_driver, target.<name>.webdriver_url, "
                    "target.<name>.debug_attach, target.<name>.debugger_command, "
                    "target.<name>.video_capture, target.<name>.frame_stats."
                )
            target_cfg["optional"] = optional_cfg
            payload_value = optional_cfg[field]
        else:
            raise ValueError(
                f"Unsupported desktop config key `{key}`. Use one of: artifact_root, publish_mode, publish_branch, retention_days, or target.<name>.<field>."
            )
        normalized = normalize_desktop_config_fn(config)
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    save_config_fn(normalized)
    if key.startswith("target."):
        _, target_name, field = key.split(".")
        payload_value = normalized["desktop_automation"]["targets"][target_name]["optional"][field]
    else:
        payload_value = normalized["desktop_automation"][key]
    payload = {
        "key": key,
        "value": payload_value,
        "config_path": str(config_path_fn()),
    }
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
        return 0

    _print_lines(desktop_config_update_lines_fn(payload), print_fn=print_fn)
    return 0


def cmd_desktop_config(
    args: argparse.Namespace,
    *,
    commands: Mapping[str, Callable[[argparse.Namespace], int]],
    print_fn: Callable[[str], None] = print,
) -> int:
    handler = commands.get(args.desktop_config_command)
    if handler is None:
        print_fn("Error: desktop config subcommand required (show, set)")
        return 1
    return handler(args)


def cmd_desktop_recent(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    desktop_run_summary_fn: Callable[[dict, dict], dict],
    desktop_recent_lines_fn: Callable[..., list[str]],
    short_sha_fn: Callable[[str], str],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    manifests = desktop_run_manifests_fn(config, target_name=args.target, action=args.action)
    if not manifests:
        print_fn("No desktop automation runs found.")
        return 0
    manifests = manifests[: args.limit]
    if getattr(args, "json", False):
        print_fn(json.dumps({"runs": manifests}, indent=2))
        return 0

    run_summaries = [desktop_run_summary_fn(config, manifest) for manifest in manifests]
    _print_lines(desktop_recent_lines_fn(run_summaries, short_sha_fn=short_sha_fn), print_fn=print_fn)
    return 0


def cmd_desktop_proof(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_proof_summaries_fn: Callable[..., list[dict]],
    desktop_proof_empty_line_fn: Callable[..., str],
    desktop_proof_lines_fn: Callable[..., list[str]],
    short_sha_fn: Callable[[str], str],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    try:
        proofs = desktop_proof_summaries_fn(
            config,
            target_name=args.target,
            action=args.action,
            source_mode=args.source_mode,
            sha=args.sha,
            branch=args.branch,
            limit=args.limit,
        )
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    if getattr(args, "json", False):
        print_fn(json.dumps({"proofs": proofs}, indent=2))
        return 0

    if not proofs:
        print_fn(
            desktop_proof_empty_line_fn(
                target=args.target,
                action=args.action,
                source_mode=args.source_mode,
                sha=args.sha,
                branch=args.branch,
                short_sha_fn=short_sha_fn,
            )
        )
        return 0

    _print_lines(desktop_proof_lines_fn(proofs, short_sha_fn=short_sha_fn), print_fn=print_fn)
    return 0


def cmd_desktop_publish(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    stage_desktop_publish_report_fn: Callable[..., dict],
    desktop_publish_lines_fn: Callable[[dict], list[str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    manifest_paths = getattr(args, "manifest", None) or []
    manifests = []
    for manifest_arg in manifest_paths:
        manifest_path = Path(manifest_arg).expanduser()
        if not manifest_path.is_file():
            print_fn(f"Error: desktop run manifest not found: {manifest_path}")
            return 1
        try:
            manifest = json.loads(manifest_path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            print_fn(f"Error: could not read desktop run manifest: {exc}")
            return 1
        artifacts = manifest.setdefault("artifacts", {})
        artifacts.setdefault("bundle_dir", str(manifest_path.parent))
        manifests.append(manifest)
    if not manifests:
        manifests = desktop_run_manifests_fn(config, target_name=args.target, action=args.action)
    if not manifests:
        print_fn("No desktop automation runs found.")
        return 0

    if not manifest_paths:
        manifests = manifests[: args.limit]
    output_dir = Path(args.output).expanduser() if args.output else None
    try:
        report = stage_desktop_publish_report_fn(config, manifests, output_dir=output_dir, label=args.label)
    except Exception as exc:
        print_fn(f"Error: {exc}")
        return 1

    if getattr(args, "json", False):
        print_fn(json.dumps(report, indent=2))
        return 0

    _print_lines(desktop_publish_lines_fn(report), print_fn=print_fn)
    return 0


def cmd_desktop_verdict(
    args: argparse.Namespace,
    *,
    now_iso_fn: Callable[[], str],
    atomic_write_text_fn: Callable[[Path, str], None],
    print_fn: Callable[[str], None] = print,
) -> int:
    manifest_path = Path(args.manifest).expanduser()
    if not manifest_path.is_file():
        print_fn(f"Error: desktop run manifest not found: {manifest_path}")
        return 1
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print_fn(f"Error: could not read desktop run manifest: {exc}")
        return 1
    status = "approved" if getattr(args, "approved", False) else "needs-work"
    review = {
        "status": status,
        "reviewed_at": now_iso_fn(),
    }
    if args.notes:
        review["notes"] = args.notes
    if args.reviewer:
        review["reviewer"] = args.reviewer
    if args.issue_url:
        review["issue_url"] = args.issue_url
    if status == "approved":
        review["close_review_issue"] = True
    else:
        review["close_review_issue"] = False
        review["follow_up_required"] = True
    manifest["review"] = review
    atomic_write_text_fn(manifest_path, json.dumps(manifest, indent=2) + "\n")
    payload = {
        "manifest": str(manifest_path),
        "review": review,
    }
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
    else:
        print_fn(f"Desktop proof verdict recorded: {status}")
        print_fn(f"  manifest: {manifest_path}")
        if args.issue_url:
            print_fn(f"  issue_url: {args.issue_url}")
    return 0


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
    title = getattr(args, "title", None) or None
    notes = [note for note in (getattr(args, "note", None) or []) if note]
    if source_image and not source_image.is_file():
        print_fn(f"Error: source image not found: {source_image}")
        return 1

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

    artifacts = manifest.setdefault("artifacts", {})
    manifest["video_composed"] = composed_summary
    manifest["video_issue"] = issue_summary
    if small_summary is not None:
        manifest["video_small"] = small_summary
    if notes:
        manifest["video_proof_notes"] = notes
    if template or source_image or source_label or title or notes:
        manifest["video_proof_composition"] = {
            "template": template or "validation-proof",
            "source_image": str(source_image) if source_image else None,
            "source_label": source_label,
            "title": title,
            "notes": notes,
        }
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


def cmd_desktop_serve(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_publish_reports_fn: Callable[..., list[dict]],
    desktop_serve_candidate_urls_fn: Callable[[str, int], list[str]] = desktop_serve_candidate_urls,
    serve_directory_fn: Callable[..., None] = serve_directory,
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    if args.path:
        serve_dir = Path(args.path).expanduser()
    else:
        reports = desktop_publish_reports_fn(config, limit=1)
        if not reports:
            print_fn("Error: no desktop publish reports found. Run `pulp ci-local desktop publish` first.")
            return 1
        serve_dir = Path(reports[0]["output_dir"]).expanduser()

    publish_root = desktop_publish_root_from_config(config)
    resolved_serve_dir = serve_dir.resolve()
    if not _path_is_relative_to(resolved_serve_dir, publish_root):
        print_fn(f"Error: desktop serve only serves reports under configured publish root: {publish_root}")
        return 1
    if not serve_dir.is_dir():
        print_fn(f"Error: desktop report directory not found: {serve_dir}")
        return 1
    if not (serve_dir / "index.html").exists():
        print_fn(f"Error: desktop report index.html not found: {serve_dir / 'index.html'}")
        return 1

    urls = desktop_serve_candidate_urls_fn(args.host, args.port)
    primary_url = urls[0] if urls else f"http://{args.host}:{args.port}/"
    print_fn(f"Serving desktop report: {primary_url}")
    for url in urls[1:]:
        print_fn(f"  also: {url}")
    print_fn(f"  directory: {serve_dir}")
    serve_directory_fn(serve_dir, host=args.host, port=args.port)
    return 0


def cmd_desktop_cleanup(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    prune_desktop_run_manifests_fn: Callable[..., list[Path]],
    write_desktop_run_rollups_fn: Callable[..., None],
    desktop_cleanup_empty_line_fn: Callable[[], str],
    desktop_cleanup_lines_fn: Callable[[list[Path]], list[str]],
    remove_tree_fn: Callable[..., None] = shutil.rmtree,
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    older_than = args.older_than_days if args.older_than_days is not None else config["desktop_automation"]["retention_days"]
    paths = prune_desktop_run_manifests_fn(
        config,
        target_name=args.target,
        older_than_days=older_than,
        keep_last=args.keep_last,
    )
    if not paths:
        print_fn(desktop_cleanup_empty_line_fn())
        return 0

    for path in paths:
        remove_tree_fn(path, ignore_errors=False)

    write_desktop_run_rollups_fn(config, target_name=args.target if args.target else None)
    if args.target is not None:
        write_desktop_run_rollups_fn(config)

    if getattr(args, "json", False):
        print_fn(json.dumps({"removed": [str(path) for path in paths]}, indent=2))
        return 0

    _print_lines(desktop_cleanup_lines_fn(paths), print_fn=print_fn)
    return 0
