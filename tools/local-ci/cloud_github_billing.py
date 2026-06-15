"""GitHub Actions provider-reported billing summary helpers."""
from __future__ import annotations

from collections.abc import Callable
from datetime import datetime, timezone


def fetch_github_repo_actions_billing_summary(
    repository: str,
    config: dict | None,
    *,
    resolve_billing_settings_fn: Callable[[dict | None], dict],
    gh_available_fn: Callable[[], bool],
    gh_api_json_fn: Callable[..., tuple[dict | None, str]],
    billing_period_window_fn: Callable[[int], tuple[datetime, datetime]],
    iter_year_months_fn: Callable[[datetime, datetime], list[tuple[int, int]]],
    gh_token_scopes_fn: Callable[[], set[str]],
    parse_iso_date_fn: Callable[[str | None], object],
    provider_billing_note_text_fn: Callable[[], str],
) -> dict:
    billing = resolve_billing_settings_fn(config)
    if not billing.get("enable_provider_reported_totals"):
        return {"status": "disabled", "reason": "disabled (opt-in)"}
    if not gh_available_fn():
        return {"status": "unavailable", "reason": "gh CLI unavailable"}

    repo_payload, repo_error = gh_api_json_fn(f"/repos/{repository}")
    if not isinstance(repo_payload, dict):
        return {
            "status": "unavailable",
            "reason": f"repo lookup failed ({repo_error or 'gh api failed'})",
        }

    owner = ((repo_payload.get("owner") or {}).get("login") or "").strip()
    owner_type = ((repo_payload.get("owner") or {}).get("type") or "").strip().lower()
    if not owner:
        return {"status": "unavailable", "reason": "repo owner unknown"}

    if owner_type == "organization":
        endpoint = f"/organizations/{owner}/settings/billing/usage"
    elif owner_type == "user":
        endpoint = f"/users/{owner}/settings/billing/usage"
    else:
        return {"status": "unavailable", "reason": f"unsupported owner type '{owner_type or 'unknown'}'"}

    period_start, period_end = billing_period_window_fn(billing["billing_period_start_day"])
    month_pairs = iter_year_months_fn(period_start, period_end)
    matched_items: list[dict] = []

    for year, month in month_pairs:
        payload, error = gh_api_json_fn(endpoint, fields={"year": year, "month": month})
        if not isinstance(payload, dict):
            reason = "GitHub billing API unavailable; check auth/platform"
            if owner_type == "user" and "user" not in gh_token_scopes_fn():
                reason = "GitHub billing API unavailable; check auth/platform"
            return {
                "status": "unavailable",
                "reason": reason,
                "detail": error,
            }
        for item in payload.get("usageItems") or []:
            if str(item.get("product", "")).strip().lower() != "actions":
                continue
            if str(item.get("repositoryName", "")).strip() != repository:
                continue
            item_date = parse_iso_date_fn(item.get("date"))
            if not item_date:
                continue
            item_dt = datetime(item_date.year, item_date.month, item_date.day, tzinfo=timezone.utc)
            if item_dt < period_start or item_dt >= period_end:
                continue
            matched_items.append(item)

    total = 0.0
    for item in matched_items:
        amount = item.get("netAmount")
        if amount in (None, ""):
            amount = item.get("grossAmount")
        try:
            total += float(amount or 0.0)
        except (TypeError, ValueError):
            continue

    return {
        "status": "actual",
        "provider": "github-hosted",
        "scope": "repo current period",
        "currency": "USD",
        "period_start": period_start.isoformat(),
        "period_end": period_end.isoformat(),
        "matched_items": len(matched_items),
        "actual_total": round(total, 4),
        "reason": provider_billing_note_text_fn(),
    }
