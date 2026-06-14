"""Compatibility facade for queue-oriented local-CI command bindings."""

from __future__ import annotations

from local_ci_drain_command_bindings import (
    LOCAL_CI_DRAIN_COMMAND_EXPORTS,
    cmd_drain,
)
from local_ci_enqueue_command_bindings import (
    LOCAL_CI_ENQUEUE_COMMAND_EXPORTS,
    cmd_enqueue,
)
from local_ci_run_command_bindings import (
    LOCAL_CI_RUN_COMMAND_EXPORTS,
    cmd_run,
)


LOCAL_CI_QUEUE_COMMAND_EXPORTS = (
    *LOCAL_CI_ENQUEUE_COMMAND_EXPORTS,
    *LOCAL_CI_DRAIN_COMMAND_EXPORTS,
    *LOCAL_CI_RUN_COMMAND_EXPORTS,
)
