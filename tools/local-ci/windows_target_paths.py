"""Windows target path and repo configuration helpers."""

from __future__ import annotations

from pathlib import PureWindowsPath


WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME = "pulp-validate"


def windows_path_join(*parts: str) -> str:
    cleaned: list[str] = []
    for index, part in enumerate(parts):
        if not part:
            continue
        piece = str(part)
        if index == 0:
            cleaned.append(piece.rstrip("\\"))
        else:
            cleaned.append(piece.strip("\\"))
    return "\\".join(cleaned)


def windows_default_repo_checkout_path(home_dir: str | None) -> str:
    home = (home_dir or "").strip()
    if not home:
        return WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME
    return windows_path_join(home, WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME)


def windows_repo_path_is_unsafe(repo_path: str | None, home_dir: str | None = None) -> bool:
    value = (repo_path or "").strip()
    if not value:
        return True
    repo = PureWindowsPath(value)
    repo_text = str(repo).rstrip("\\")
    anchor = repo.anchor.rstrip("\\")
    if not repo_text or (anchor and repo_text.lower() == anchor.lower()):
        return True

    home_value = (home_dir or "").strip()
    if home_value:
        home = PureWindowsPath(home_value)
        home_text = str(home).rstrip("\\")
        if home_text and repo_text.lower() == home_text.lower():
            return True
    return False


def update_target_repo_path(config: dict, target_name: str, repo_path: str) -> None:
    config.setdefault("targets", {}).setdefault(target_name, {})["repo_path"] = repo_path
    desktop = config.setdefault("desktop_automation", {})
    desktop_targets = desktop.setdefault("targets", {})
    desktop_targets.setdefault(target_name, {})["repo_path"] = repo_path


def windows_repo_checkout_ready(probe: dict | None) -> bool:
    if not probe:
        return False
    return (
        bool(probe.get("git_dir_exists"))
        and bool(probe.get("head_exists"))
        and bool(probe.get("setup_exists"))
        and not bool(probe.get("repo_path_unsafe"))
    )
