"""Bindings from the local_ci facade to SSH subprocess helpers."""

from __future__ import annotations

from collections.abc import Mapping
import subprocess
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


SSH_SUBPROCESS_EXPORTS = (
    "is_transient_ssh_failure_detail",
    "run_ssh_subprocess",
)


def is_transient_ssh_failure_detail(bindings: Mapping[str, Any], detail: str) -> bool:
    return _binding(bindings, "_ssh_subprocess").is_transient_ssh_failure_detail(detail)


def run_ssh_subprocess(
    bindings: Mapping[str, Any],
    args: list[str],
    *,
    input: str | None = None,
    timeout: int | None = None,
    retries: int = 3,
    retry_delay_secs: float = 2.0,
) -> subprocess.CompletedProcess[str]:
    return _binding(bindings, "_ssh_subprocess").run_ssh_subprocess(
        args,
        input=input,
        timeout=timeout,
        retries=retries,
        retry_delay_secs=retry_delay_secs,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
    )


def install_ssh_subprocess_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = SSH_SUBPROCESS_EXPORTS,
) -> None:
    known_names = set(SSH_SUBPROCESS_EXPORTS)
    subprocess_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), subprocess_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
