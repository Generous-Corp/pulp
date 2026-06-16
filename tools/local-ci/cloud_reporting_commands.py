"""Compatibility facade for cloud reporting/defaults/status commands."""
from __future__ import annotations

from cloud_reporting_compare import cmd_cloud_compare, cmd_cloud_recommend
from cloud_reporting_defaults import cmd_cloud_defaults
from cloud_reporting_history import cmd_cloud_history
from cloud_reporting_status import cmd_cloud_status
from cloud_reporting_workflows import cmd_cloud_workflows


__all__ = [
    "cmd_cloud_compare",
    "cmd_cloud_defaults",
    "cmd_cloud_history",
    "cmd_cloud_recommend",
    "cmd_cloud_status",
    "cmd_cloud_workflows",
]
