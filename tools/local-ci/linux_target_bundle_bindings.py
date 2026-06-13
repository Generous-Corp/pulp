"""Bindings from the local_ci facade to Linux remote bundle path helpers."""

from __future__ import annotations

from pathlib import Path

from binding_utils import binding as _binding


LINUX_TARGET_BUNDLE_EXPORTS = ("remote_linux_bundle_relpath",)


def remote_linux_bundle_relpath(bindings: dict, target_name: str, action_name: str, bundle_dir: Path) -> str:
    return _binding(bindings, "_linux_target").remote_linux_bundle_relpath(target_name, action_name, bundle_dir)
