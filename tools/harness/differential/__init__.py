"""Browser/native differential compatibility lab contracts."""

from .contract import (
    DifferentialReport,
    FixtureSpec,
    load_fixture_manifest,
    normalize_report,
    validate_report,
)

__all__ = [
    "DifferentialReport",
    "FixtureSpec",
    "load_fixture_manifest",
    "normalize_report",
    "validate_report",
]
