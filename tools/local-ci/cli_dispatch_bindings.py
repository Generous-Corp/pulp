"""Compatibility facade for CLI dispatch dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from cli_desktop_dispatch_bindings import (
    CLI_DESKTOP_DISPATCH_EXPORTS,
    cmd_desktop,
    cmd_desktop_config,
    install_cli_desktop_dispatch_helpers,
)
from cli_main_dispatch_bindings import (
    CLI_MAIN_DISPATCH_EXPORTS,
    dispatch_main_command,
    install_cli_main_dispatch_helpers,
)


CLI_DISPATCH_EXPORTS = (
    *CLI_DESKTOP_DISPATCH_EXPORTS,
    *CLI_MAIN_DISPATCH_EXPORTS,
)


def install_cli_dispatch_helpers(bindings: dict[str, Any], names: tuple[str, ...] = CLI_DISPATCH_EXPORTS) -> None:
    desktop_names = tuple(name for name in names if name in CLI_DESKTOP_DISPATCH_EXPORTS)
    main_names = tuple(name for name in names if name in CLI_MAIN_DISPATCH_EXPORTS)
    known_names = set(CLI_DISPATCH_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_cli_desktop_dispatch_helpers(bindings, desktop_names)
    install_cli_main_dispatch_helpers(bindings, main_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
