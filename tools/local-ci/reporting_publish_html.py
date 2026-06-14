"""HTML rendering for desktop automation publish reports."""

from __future__ import annotations

import html
import json


def desktop_publish_run_card_html(run: dict) -> str:
    artifacts = run["artifacts"]
    screenshot = artifacts.get("screenshot")
    before = artifacts.get("before_screenshot")
    diff = artifacts.get("diff_screenshot")
    meta_lines = [
        f"<div><strong>{html.escape(str(run.get('target') or '?'))}/{html.escape(str(run.get('action') or '?'))}</strong></div>",
        f"<div>{html.escape(str(run.get('label') or '?'))}</div>",
    ]
    if run.get("completed_at"):
        meta_lines.append(f"<div>{html.escape(str(run['completed_at']))}</div>")
    if run.get("interaction_mode"):
        meta_lines.append(f"<div>interaction: {html.escape(str(run['interaction_mode']))}</div>")
    if artifacts.get("image_change"):
        meta_lines.append(f"<div>image_change: {html.escape(json.dumps(artifacts['image_change'], sort_keys=True))}</div>")

    image_blocks: list[str] = []
    for title, rel_path in (("before", before), ("after", screenshot), ("diff", diff)):
        if not rel_path:
            continue
        image_blocks.append(
            "<figure>"
            f"<figcaption>{html.escape(title)}</figcaption>"
            f"<img src=\"{html.escape(str(rel_path))}\" alt=\"{html.escape(title)}\" />"
            "</figure>"
        )
    return (
        "<section class=\"run-card\">"
        + "".join(meta_lines)
        + "<div class=\"images\">"
        + "".join(image_blocks)
        + "</div></section>"
    )


def desktop_publish_index_html(index_payload: dict) -> str:
    published_runs = index_payload["runs"]
    cards = [desktop_publish_run_card_html(run) for run in published_runs]
    return (
        "\n".join(
            [
                "<!doctype html>",
                "<html><head><meta charset=\"utf-8\"><title>Pulp Desktop Automation Report</title>",
                "<style>body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;margin:24px;background:#111827;color:#e5e7eb}"
                " .run-card{border:1px solid #374151;border-radius:12px;padding:16px;margin:0 0 16px;background:#1f2937}"
                " .images{display:flex;gap:16px;flex-wrap:wrap;margin-top:12px}"
                " figure{margin:0} figcaption{margin-bottom:8px;color:#9ca3af} img{max-width:320px;border-radius:8px;border:1px solid #374151;background:#000}</style>",
                "</head><body>",
                f"<h1>{html.escape(index_payload['label'])}</h1>",
                f"<p>Generated at {html.escape(index_payload['generated_at'])} &middot; runs: {len(published_runs)}</p>",
                *cards,
                "</body></html>",
            ]
        )
        + "\n"
    )
