"""Cloud billing configuration normalization helpers."""
from __future__ import annotations


def parse_rate_value(value) -> float | None:
    if value in (None, ""):
        return None
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    if parsed < 0:
        return None
    return parsed


def parse_optional_bool(value, setting_name: str) -> bool | None:
    if value in (None, ""):
        return None
    if isinstance(value, bool):
        return value
    raise ValueError(f"{setting_name} must be true or false.")


def resolve_billing_settings(config: dict | None) -> dict:
    billing = (((config or {}).get("telemetry") or {}).get("billing") or {})
    settings = {
        "currency": "USD",
        "billing_period_start_day": 1,
        "enable_provider_reported_totals": False,
        "github_hosted_job_os_rates_per_minute": {},
        "namespace_profile_tag_rates_per_hour": {},
        "namespace_machine_shape_rates_per_hour": [],
    }
    if not isinstance(billing, dict):
        return settings

    currency = billing.get("currency")
    if isinstance(currency, str) and currency.strip():
        settings["currency"] = currency.strip().upper()

    start_day = billing.get("billing_period_start_day")
    if start_day not in (None, ""):
        try:
            parsed_start_day = int(start_day)
        except (TypeError, ValueError) as exc:
            raise ValueError("telemetry.billing.billing_period_start_day must be an integer.") from exc
        if parsed_start_day < 1 or parsed_start_day > 28:
            raise ValueError("telemetry.billing.billing_period_start_day must be between 1 and 28.")
        settings["billing_period_start_day"] = parsed_start_day

    provider_reported_totals = parse_optional_bool(
        billing.get("enable_provider_reported_totals"),
        "telemetry.billing.enable_provider_reported_totals",
    )
    if provider_reported_totals is not None:
        settings["enable_provider_reported_totals"] = provider_reported_totals

    github_rates = billing.get("github_hosted_job_os_rates_per_minute")
    if isinstance(github_rates, dict):
        for os_name, value in github_rates.items():
            if not isinstance(os_name, str) or not os_name.strip():
                continue
            parsed = parse_rate_value(value)
            if parsed is not None:
                settings["github_hosted_job_os_rates_per_minute"][os_name.strip().lower()] = parsed

    namespace_profile_rates = billing.get("namespace_profile_tag_rates_per_hour")
    if isinstance(namespace_profile_rates, dict):
        for tag, value in namespace_profile_rates.items():
            if not isinstance(tag, str) or not tag.strip():
                continue
            parsed = parse_rate_value(value)
            if parsed is not None:
                settings["namespace_profile_tag_rates_per_hour"][tag.strip()] = parsed

    shape_rates = billing.get("namespace_machine_shape_rates_per_hour")
    if isinstance(shape_rates, list):
        normalized_shape_rates = []
        for raw in shape_rates:
            if not isinstance(raw, dict):
                continue
            parsed_rate = parse_rate_value(raw.get("rate"))
            if parsed_rate is None:
                continue
            normalized_shape_rates.append(
                {
                    "os": str(raw.get("os", "")).strip().lower(),
                    "arch": str(raw.get("arch", "")).strip().lower(),
                    "virtual_cpu": int(raw.get("virtual_cpu") or 0),
                    "memory_megabytes": int(raw.get("memory_megabytes") or 0),
                    "rate": parsed_rate,
                }
            )
        settings["namespace_machine_shape_rates_per_hour"] = normalized_shape_rates

    return settings
