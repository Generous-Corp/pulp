"""Shared helpers for local_ci facade binding modules."""

from __future__ import annotations

import builtins
from collections.abc import Mapping
from functools import update_wrapper
from typing import Any


def binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def binding_attr(bindings: Mapping[str, Any], name: str, attribute: str) -> Any:
    return getattr(binding(bindings, name), attribute)


def print_binding(bindings: Mapping[str, Any]) -> Any:
    return bindings.get("print", builtins.print)


def bind_local_helper(bindings: Mapping[str, Any], namespace: Mapping[str, Any], name: str):
    target = namespace[name]

    def facade(*args, **kwargs):
        return target(bindings, *args, **kwargs)

    return update_wrapper(facade, target)


def install_local_helpers(bindings: dict[str, Any], namespace: Mapping[str, Any], names: tuple[str, ...]) -> None:
    for name in names:
        bindings[name] = bind_local_helper(bindings, namespace, name)


def bind_module_attr(bindings: Mapping[str, Any], module_name: str, name: str):
    helper = binding_attr(bindings, module_name, name)

    def facade(*args, **kwargs):
        return binding_attr(bindings, module_name, name)(*args, **kwargs)

    return update_wrapper(facade, helper)


def install_module_attrs(bindings: dict[str, Any], module_name: str, names: tuple[str, ...]) -> None:
    for name in names:
        bindings[name] = bind_module_attr(bindings, module_name, name)
