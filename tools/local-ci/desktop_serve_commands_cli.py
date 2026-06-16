"""Desktop report HTTP serve command (serve a published report over local HTTP)."""

from __future__ import annotations

import argparse
from collections.abc import Callable, Mapping
from datetime import datetime, timezone
import functools
import http.server
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time
import urllib.error
import urllib.request


def _append_unique(items: list[str], value: str | None) -> None:
    value = (value or "").strip()
    if value and value not in items:
        items.append(value)


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


def desktop_serve_state_dir(publish_root: Path) -> Path:
    return publish_root / "_serve"


def desktop_serve_state_path(publish_root: Path, label: str) -> Path:
    safe_label = "".join(ch if ch.isalnum() or ch in {"-", "_", "."} else "-" for ch in label.strip())
    return desktop_serve_state_dir(publish_root) / f"{safe_label or 'desktop-proof'}.json"


def process_is_running(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True




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


def verify_desktop_serve_url(
    url: str,
    *,
    timeout: float = 2.0,
    urlopen_fn: Callable[..., object] = urllib.request.urlopen,
) -> dict:
    payload = {
        "kind": "desktop-proof-serve-verification",
        "url": url,
        "checked_at": datetime.now(timezone.utc).isoformat(),
    }
    try:
        response = urlopen_fn(url, timeout=timeout)
        try:
            status = int(getattr(response, "status", 200) or 200)
            payload.update(
                {
                    "status": "ok" if 200 <= status < 400 else "failed",
                    "http_status": status,
                }
            )
        finally:
            close = getattr(response, "close", None)
            if callable(close):
                close()
    except urllib.error.HTTPError as exc:
        payload.update({"status": "failed", "http_status": int(exc.code), "error": str(exc)})
    except Exception as exc:
        payload.update({"status": "failed", "error": str(exc)})
    return payload


def find_available_desktop_serve_port(host: str, preferred_port: int, *, attempts: int = 100) -> int:
    start = max(1, int(preferred_port or 8765))
    for offset in range(max(1, attempts)):
        candidate = start + offset
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.bind((host, candidate))
            return candidate
        except OSError:
            continue
    raise RuntimeError(f"no free desktop proof serve port found starting at {start}")


def _write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n")


def _read_text_tail(path: Path, *, max_chars: int = 4000) -> str:
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return ""
    if len(text) <= max_chars:
        return text
    return text[-max_chars:]


def start_desktop_serve_process(
    serve_dir: Path,
    *,
    host: str,
    port: int,
    label: str,
    publish_root: Path,
    urls: list[str],
    popen_fn: Callable[..., subprocess.Popen] = subprocess.Popen,
    now_fn: Callable[[], float] = time.time,
    sleep_fn: Callable[[float], None] = time.sleep,
    startup_grace_seconds: float = 0.2,
) -> dict:
    state_path = desktop_serve_state_path(publish_root, label)
    log_dir = desktop_serve_state_dir(publish_root)
    log_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = log_dir / f"{state_path.stem}.out"
    stderr_path = log_dir / f"{state_path.stem}.err"
    stdout_handle = stdout_path.open("a")
    stderr_handle = stderr_path.open("a")
    try:
        process = popen_fn(
            [
                sys.executable,
                "-u",
                "-m",
                "http.server",
                str(port),
                "--bind",
                host,
                "--directory",
                str(serve_dir),
            ],
            stdout=stdout_handle,
            stderr=stderr_handle,
            start_new_session=True,
        )
    finally:
        stdout_handle.close()
        stderr_handle.close()

    payload = {
        "kind": "desktop-proof-serve-process",
        "label": label,
        "pid": int(process.pid),
        "host": host,
        "port": port,
        "directory": str(serve_dir),
        "urls": urls,
        "state_path": str(state_path),
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
        "started_at_epoch": now_fn(),
        "command": [
            sys.executable,
            "-u",
            "-m",
            "http.server",
            str(port),
            "--bind",
            host,
            "--directory",
            str(serve_dir),
        ],
    }
    if startup_grace_seconds > 0:
        sleep_fn(startup_grace_seconds)
    exit_code = process.poll()
    if exit_code is not None:
        payload.update(
            {
                "status": "failed",
                "exit_code": int(exit_code),
                "error": "desktop proof server exited during startup",
                "stdout_tail": _read_text_tail(stdout_path),
                "stderr_tail": _read_text_tail(stderr_path),
            }
        )
    _write_json(state_path, payload)
    return payload


def persist_desktop_serve_urls(report_dir: Path, urls: list[str], verification: dict | None = None) -> None:
    clean_urls = [str(url) for url in urls if str(url).strip()]
    if not clean_urls and not verification:
        return
    for filename in ("index.json", "review-package.json"):
        path = report_dir / filename
        if not path.exists():
            continue
        try:
            payload = json.loads(path.read_text())
        except (json.JSONDecodeError, OSError):
            continue
        if not isinstance(payload, dict):
            continue
        if clean_urls:
            payload["serve_urls"] = clean_urls
        if verification:
            payload["serve_verification"] = verification
        for run in payload.get("runs") or []:
            if not isinstance(run, dict):
                continue
            fallback = run.get("fallback")
            if isinstance(fallback, dict):
                if clean_urls:
                    fallback["serve_urls"] = clean_urls
                if verification:
                    fallback["serve_verification"] = verification
        _write_json(path, payload)


def read_desktop_serve_state(publish_root: Path, label: str) -> dict | None:
    state_path = desktop_serve_state_path(publish_root, label)
    if not state_path.exists():
        return None
    try:
        return json.loads(state_path.read_text())
    except json.JSONDecodeError:
        return {"kind": "desktop-proof-serve-process", "label": label, "state_path": str(state_path), "invalid": True}


def stop_desktop_serve_process(
    publish_root: Path,
    label: str,
    *,
    is_running_fn: Callable[[int], bool] = process_is_running,
    kill_fn: Callable[[int, int], None] = os.kill,
    sleep_fn: Callable[[float], None] = time.sleep,
) -> dict:
    state_path = desktop_serve_state_path(publish_root, label)
    state = read_desktop_serve_state(publish_root, label)
    if not state:
        return {"status": "missing", "label": label, "state_path": str(state_path)}
    pid = int(state.get("pid") or 0)
    was_running = is_running_fn(pid)
    if was_running:
        kill_fn(pid, signal.SIGTERM)
        for _ in range(10):
            sleep_fn(0.1)
            if not is_running_fn(pid):
                break
    stopped = not is_running_fn(pid)
    if stopped:
        state_path.unlink(missing_ok=True)
    return {
        "status": "stopped" if stopped else "still-running",
        "label": label,
        "pid": pid,
        "was_running": was_running,
        "state_path": str(state_path),
    }




def cmd_desktop_serve(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_publish_reports_fn: Callable[..., list[dict]],
    desktop_serve_candidate_urls_fn: Callable[[str, int], list[str]] = desktop_serve_candidate_urls,
    serve_directory_fn: Callable[..., None] = serve_directory,
    start_serve_process_fn: Callable[..., dict] = start_desktop_serve_process,
    persist_serve_urls_fn: Callable[..., None] = persist_desktop_serve_urls,
    read_serve_state_fn: Callable[[Path, str], dict | None] = read_desktop_serve_state,
    stop_serve_process_fn: Callable[..., dict] = stop_desktop_serve_process,
    is_running_fn: Callable[[int], bool] = process_is_running,
    find_available_port_fn: Callable[[str, int], int] = find_available_desktop_serve_port,
    verify_serve_url_fn: Callable[[str], dict] = verify_desktop_serve_url,
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    publish_root = desktop_publish_root_from_config(config)
    label = getattr(args, "label", None) or "desktop-proof"

    if getattr(args, "stop", False):
        result = stop_serve_process_fn(publish_root, label)
        if getattr(args, "json", False):
            print_fn(json.dumps(result, indent=2))
        else:
            print_fn(f"Desktop proof server {result['status']}: {label}")
            if result.get("pid"):
                print_fn(f"  pid: {result['pid']}")
        return 0 if result.get("status") in {"stopped", "missing"} else 1

    if getattr(args, "status", False):
        state = read_serve_state_fn(publish_root, label)
        if not state:
            result = {"status": "missing", "label": label, "state_path": str(desktop_serve_state_path(publish_root, label))}
        else:
            pid = int(state.get("pid") or 0)
            if is_running_fn(pid):
                status = "running"
            elif state.get("status") == "failed":
                status = "failed"
            else:
                status = "stale"
            result = {**state, "status": status}
        if getattr(args, "json", False):
            print_fn(json.dumps(result, indent=2))
        else:
            print_fn(f"Desktop proof server {result['status']}: {label}")
            if result.get("pid"):
                print_fn(f"  pid: {result['pid']}")
            for url in result.get("urls") or []:
                print_fn(f"  url: {url}")
            if result.get("directory"):
                print_fn(f"  directory: {result['directory']}")
        return 0 if result.get("status") in {"running", "missing", "stale"} else 1

    if args.path:
        serve_dir = Path(args.path).expanduser()
    else:
        reports = desktop_publish_reports_fn(config, limit=1)
        if not reports:
            print_fn("Error: no desktop publish reports found. Run `pulp ci-local desktop publish` first.")
            return 1
        serve_dir = Path(reports[0]["output_dir"]).expanduser()

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

    port = int(getattr(args, "port", 8765) or 8765)
    if getattr(args, "auto_port", False):
        try:
            port = int(find_available_port_fn(args.host, port))
        except Exception as exc:
            print_fn(f"Error: {exc}")
            return 1

    urls = desktop_serve_candidate_urls_fn(args.host, port)
    primary_url = urls[0] if urls else f"http://{args.host}:{port}/"
    if getattr(args, "background", False):
        state = start_serve_process_fn(
            resolved_serve_dir,
            host=args.host,
            port=port,
            label=label,
            publish_root=publish_root,
            urls=urls,
        )
        if state.get("status") == "failed":
            if getattr(args, "json", False):
                print_fn(json.dumps(state, indent=2))
            else:
                print_fn(f"Error: desktop proof server failed to start: {label}")
                if state.get("exit_code") is not None:
                    print_fn(f"  exit_code: {state['exit_code']}")
                if state.get("stderr_tail"):
                    print_fn(f"  stderr: {state['stderr_tail'].strip()}")
                if state.get("stdout_tail"):
                    print_fn(f"  stdout: {state['stdout_tail'].strip()}")
            return 1
        verification = verify_serve_url_fn(primary_url)
        state["serve_verification"] = verification
        state_path = state.get("state_path")
        if state_path:
            _write_json(Path(str(state_path)), state)
        persist_serve_urls_fn(resolved_serve_dir, urls, verification)
        if getattr(args, "json", False):
            print_fn(json.dumps({**state, "status": "started"}, indent=2))
        else:
            print_fn(f"Serving desktop report: {primary_url}")
            for url in urls[1:]:
                print_fn(f"  also: {url}")
            print_fn(f"  directory: {serve_dir}")
            print_fn(f"  background: {label}")
            print_fn(f"  pid: {state['pid']}")
            if port != int(getattr(args, "port", 8765) or 8765):
                print_fn(f"  auto_port: {port}")
            print_fn(f"  verification: {verification.get('status')} {verification.get('url')}")
            print_fn(f"  stop: python3 tools/local-ci/local_ci.py desktop serve --stop --label {label}")
        return 0
    print_fn(f"Serving desktop report: {primary_url}")
    for url in urls[1:]:
        print_fn(f"  also: {url}")
    print_fn(f"  directory: {serve_dir}")
    persist_serve_urls_fn(resolved_serve_dir, urls)
    serve_directory_fn(serve_dir, host=args.host, port=port)
    return 0


