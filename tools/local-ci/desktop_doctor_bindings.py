"""Bindings from the local_ci facade to desktop doctor helpers."""

from __future__ import annotations

from pathlib import Path

from binding_utils import binding as _binding


DESKTOP_DOCTOR_EXPORTS = (
    "desktop_optional_capabilities",
    "desktop_capabilities_for",
    "desktop_check",
    "check_writable_dir",
    "webdriver_status_url",
)


def desktop_optional_capabilities(bindings: dict, optional_cfg: dict | None) -> list[str]:
    return _binding(bindings, "_desktop_doctor").desktop_optional_capabilities(optional_cfg)


def desktop_capabilities_for(bindings: dict, adapter: str, tier: str, optional_cfg: dict | None = None) -> list[str]:
    return _binding(bindings, "_desktop_doctor").desktop_capabilities_for(adapter, tier, optional_cfg)


def desktop_check(bindings: dict, name: str, ok: bool, detail: str, *, required: bool = True) -> dict:
    return _binding(bindings, "_desktop_doctor").desktop_check(name, ok, detail, required=required)


def check_writable_dir(bindings: dict, path: Path) -> tuple[bool, str]:
    return _binding(bindings, "_desktop_doctor").check_writable_dir(path)


def webdriver_status_url(bindings: dict, base_url: str) -> str:
    return _binding(bindings, "_desktop_doctor").webdriver_status_url(base_url)
