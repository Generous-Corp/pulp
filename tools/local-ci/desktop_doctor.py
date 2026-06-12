"""Desktop automation doctor and optional probe helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import shutil
import sys
import uuid

from desktop_doctor_checks import (
    macos_local_doctor_checks,
    optional_desktop_doctor_checks,
    ssh_desktop_doctor_checks,
)
from desktop_doctor_optional import (
    desktop_capabilities_for,
    desktop_optional_capabilities,
    probe_webdriver_endpoint,
    webdriver_status_url,
)


def desktop_check(name: str, ok: bool, detail: str, *, required: bool = True) -> dict:
    return {"name": name, "ok": ok, "detail": detail, "required": required}


def check_writable_dir(path: Path) -> tuple[bool, str]:
    probe = None
    try:
        path.mkdir(parents=True, exist_ok=True)
        probe = path / f".write-check-{uuid.uuid4().hex}"
        probe.write_text("ok\n")
        return True, str(path)
    except OSError as exc:
        return False, str(exc)
    finally:
        if probe is not None:
            try:
                probe.unlink(missing_ok=True)
            except OSError:
                pass


def desktop_doctor_checks(
    config: dict,
    target_name: str,
    *,
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    desktop_target_contract_fn: Callable[[str, dict], dict],
    desktop_receipt_for_fn: Callable[[str], dict | None],
    macos_accessibility_trusted_fn: Callable[[], bool],
    ssh_reachable_fn: Callable[[str, int], bool],
    ssh_failure_detail_fn: Callable[[str, int], str],
    probe_linux_launch_backend_fn: Callable[[str], dict],
    probe_linux_remote_tooling_fn: Callable[[str], dict],
    probe_windows_session_agent_fn: Callable[[str, dict], dict],
    probe_windows_remote_tooling_fn: Callable[[str], dict],
    probe_windows_repo_checkout_fn: Callable[[str, str | None], dict],
    platform: str | None = None,
    which_fn: Callable[[str], str | None] | None = None,
    probe_webdriver_endpoint_fn: Callable[..., dict] | None = None,
) -> list[dict]:
    platform = platform or sys.platform
    which_fn = which_fn or shutil.which
    desktop_cfg = config["desktop_automation"]
    target = resolve_desktop_target_fn(config, target_name)
    contract = desktop_target_contract_fn(target_name, target)
    checks: list[dict] = []

    ok, detail = check_writable_dir(Path(desktop_cfg["artifact_root"]))
    checks.append(desktop_check("artifact_root", ok, detail))

    receipt = desktop_receipt_for_fn(target_name)
    checks.append(
        desktop_check(
            "receipt",
            receipt is not None,
            "installed" if receipt else f"not installed; run `pulp ci-local desktop install {target_name}`",
        )
    )

    adapter = target["adapter"]
    if adapter == "macos-local":
        checks.extend(
            macos_local_doctor_checks(
                platform=platform,
                which_fn=which_fn,
                macos_accessibility_trusted_fn=macos_accessibility_trusted_fn,
                desktop_check_fn=desktop_check,
            )
        )
    elif target["target_type"] == "ssh":
        checks.extend(
            ssh_desktop_doctor_checks(
                target_name=target_name,
                target=target,
                contract=contract,
                receipt=receipt,
                ssh_reachable_fn=ssh_reachable_fn,
                ssh_failure_detail_fn=ssh_failure_detail_fn,
                probe_linux_launch_backend_fn=probe_linux_launch_backend_fn,
                probe_linux_remote_tooling_fn=probe_linux_remote_tooling_fn,
                probe_windows_session_agent_fn=probe_windows_session_agent_fn,
                probe_windows_remote_tooling_fn=probe_windows_remote_tooling_fn,
                probe_windows_repo_checkout_fn=probe_windows_repo_checkout_fn,
                desktop_check_fn=desktop_check,
            )
        )
    else:
        checks.append(desktop_check("adapter", adapter != "unknown", adapter))

    probe_webdriver_endpoint_fn = probe_webdriver_endpoint_fn or probe_webdriver_endpoint
    checks.extend(
        optional_desktop_doctor_checks(
            target,
            which_fn=which_fn,
            probe_webdriver_endpoint_fn=probe_webdriver_endpoint_fn,
            desktop_check_fn=desktop_check,
        )
    )

    return checks
