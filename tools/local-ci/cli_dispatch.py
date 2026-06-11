"""Command dispatch helpers for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable, Mapping


CommandHandler = Callable[[argparse.Namespace], int]


def dispatch_desktop_command(
    args: argparse.Namespace,
    *,
    commands: Mapping[str, CommandHandler],
    print_fn: Callable[[str], None] = print,
) -> int:
    handler = commands.get(args.desktop_command)
    if handler is None:
        print_fn("Error: desktop subcommand required (install, doctor, status, config, recent, proof, publish, cleanup, smoke, click, inspect)")
        return 1
    return handler(args)


def dispatch_main_command(
    args: argparse.Namespace,
    *,
    commands: Mapping[str, CommandHandler],
    cloud_commands: Mapping[str, CommandHandler],
    cloud_namespace_commands: Mapping[str, CommandHandler],
    print_help: Callable[[], None],
    print_fn: Callable[[str], None] = print,
) -> int:
    if args.command == "cloud":
        if args.cloud_command == "namespace":
            namespace_handler = cloud_namespace_commands.get(args.cloud_namespace_command)
            if namespace_handler is None:
                print_fn("Error: missing cloud namespace subcommand. Use `pulp ci-local cloud namespace doctor`.")
                return 1
            return namespace_handler(args)

        cloud_handler = cloud_commands.get(args.cloud_command)
        if cloud_handler is None:
            print_fn("Error: missing cloud subcommand. Use `pulp ci-local cloud workflows`.")
            return 1
        return cloud_handler(args)

    handler = commands.get(args.command)
    if handler is not None:
        return handler(args)

    print_help()
    return 1
