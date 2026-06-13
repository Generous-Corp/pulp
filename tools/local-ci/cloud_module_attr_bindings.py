"""Installer for cloud helpers that remain direct module-attribute bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_module_attrs
from cloud_billing_attr_bindings import (
    CLOUD_BILLING_EXPORTS,
    install_cloud_billing_attr_helpers,
)
from cloud_format_attr_bindings import (
    CLOUD_FORMAT_EXPORTS,
    install_cloud_format_attr_helpers,
)
from cloud_github_attr_bindings import (
    CLOUD_GITHUB_MODULE_EXPORTS,
    install_cloud_github_attr_helpers,
)
from cloud_namespace_attr_bindings import (
    CLOUD_NAMESPACE_EXPORTS,
    install_cloud_namespace_attr_helpers,
)
from cloud_record_store_attr_bindings import (
    CLOUD_RECORD_STORE_EXPORTS,
    install_cloud_record_store_attr_helpers,
)

CLOUD_MODULE_ATTR_EXPORTS = (
    *CLOUD_BILLING_EXPORTS,
    *CLOUD_RECORD_STORE_EXPORTS,
    *CLOUD_GITHUB_MODULE_EXPORTS,
    *CLOUD_NAMESPACE_EXPORTS,
    *CLOUD_FORMAT_EXPORTS,
)


def install_cloud_module_attr_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_MODULE_ATTR_EXPORTS,
) -> None:
    billing_names = tuple(name for name in names if name in CLOUD_BILLING_EXPORTS)
    record_store_names = tuple(name for name in names if name in CLOUD_RECORD_STORE_EXPORTS)
    github_names = tuple(name for name in names if name in CLOUD_GITHUB_MODULE_EXPORTS)
    namespace_names = tuple(name for name in names if name in CLOUD_NAMESPACE_EXPORTS)
    format_names = tuple(name for name in names if name in CLOUD_FORMAT_EXPORTS)
    known_names = set(CLOUD_MODULE_ATTR_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_cloud_billing_attr_helpers(bindings, billing_names)
    install_cloud_record_store_attr_helpers(bindings, record_store_names)
    install_cloud_github_attr_helpers(bindings, github_names)
    install_cloud_namespace_attr_helpers(bindings, namespace_names)
    install_cloud_format_attr_helpers(bindings, format_names)
    if unknown_names:
        install_module_attrs(bindings, "_cloud", unknown_names)
