"""Desktop automation action selector helpers."""

from __future__ import annotations

import argparse


def windows_requires_pulp_app_selectors(args: argparse.Namespace) -> bool:
    return any([args.click_view_id, args.click_view_type, args.click_view_text, args.click_view_label])


def desktop_click_has_target(args: argparse.Namespace) -> bool:
    return any([args.click, args.click_view_id, args.click_view_type, args.click_view_text, args.click_view_label])
