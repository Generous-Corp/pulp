"""Compatibility facade for CLI dispatch dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from cli_desktop_dispatch_bindings import (
    CLI_DESKTOP_DISPATCH_EXPORTS,
    cmd_desktop,
    cmd_desktop_config,
)
from cli_main_dispatch_bindings import (
    CLI_MAIN_DISPATCH_EXPORTS,
    dispatch_main_command,
)


CLI_DISPATCH_EXPORTS = (
    *CLI_DESKTOP_DISPATCH_EXPORTS,
    *CLI_MAIN_DISPATCH_EXPORTS,
)


def install_cli_dispatch_helpers(bindings: dict[str, Any], names: tuple[str, ...] = CLI_DISPATCH_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
