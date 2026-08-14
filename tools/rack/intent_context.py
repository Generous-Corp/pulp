"""Resolve optional Rack-library anchors without replacing natural language.

``@`` names a maker or a module elsewhere in the generator.  This module owns
the companion ``#`` path: a tag is resolved from the metadata already present
in the local library index, rather than from a second Forge-maintained list.
Explicit tags are requirements; ordinary tag words are retrieval cues only.
"""

from __future__ import annotations

from dataclasses import dataclass
import re


@dataclass(frozen=True)
class TagReference:
    tag: str
    explicit: bool


def _fold(text: str) -> str:
    return "".join(char for char in text.casefold() if char.isalnum())


def _tag_matches(query: str, tag: str) -> bool:
    """Match a tag as people type it, including a simple final plural."""
    left, right = _fold(query), _fold(tag)
    if left == right:
        return True
    if len(left) > 3 and len(right) > 3:
        return left.rstrip("s") == right.rstrip("s")
    return False


def canonical_tags(inv: dict, module_index: dict) -> list[str]:
    """Every published tag, with installed metadata filling index gaps."""
    seen: dict[str, str] = {}
    for source in (module_index, inv):
        for package in source.values():
            for module in (package.get("modules") or {}).values():
                for tag in module.get("tags") or []:
                    if isinstance(tag, str) and tag.strip():
                        seen.setdefault(_fold(tag), tag.strip())
    return sorted(seen.values(), key=lambda tag: (-len(_fold(tag)), tag.casefold()))


def _tag_pattern(tag: str, marker: bool) -> re.Pattern[str]:
    words = re.findall(r"[\w]+", tag, flags=re.UNICODE)
    body = r"[\s_-]+".join(re.escape(word) for word in words)
    # VCV's names are singular in the common case. Accept a straightforward
    # plural without trying to turn this into an English grammar engine.
    suffix = r"(?:s|es)?" if words and not words[-1].casefold().endswith("s") else ""
    prefix = r"#\s*" if marker else r"(?<![\w#])"
    return re.compile(prefix + body + suffix + r"(?!\w)", re.IGNORECASE)


def resolve_tag_references(prompt: str, inv: dict, module_index: dict) -> list[TagReference]:
    """Return explicit ``#`` tags and ordinary-language retrieval cues.

    A plain word such as "sampler" is useful evidence for retrieval, but it is
    not silently upgraded into a promise that the final patch must contain one.
    A leading ``#`` is the deliberate, inspectable way to make that promise.
    """
    references: dict[str, TagReference] = {}
    tags = canonical_tags(inv, module_index)
    for explicit in (True, False):
        for tag in tags:
            if not _tag_pattern(tag, explicit).search(prompt):
                continue
            key = _fold(tag)
            previous = references.get(key)
            if previous is None or explicit:
                references[key] = TagReference(tag, explicit)
    return sorted(references.values(), key=lambda ref: (not ref.explicit, ref.tag.casefold()))


def _has_tag(module: dict, tag: str) -> bool:
    return any(_tag_matches(str(actual), tag) for actual in module.get("tags") or [])


def candidates(references: list[TagReference], inv: dict,
               limit: int = 6) -> dict[str, list[tuple[str, str, dict]]]:
    """Installed, usable examples for each requested or inferred tag."""
    out: dict[str, list[tuple[str, str, dict]]] = {}
    for reference in references:
        rows = []
        for plugin, package in inv.items():
            for model, module in (package.get("modules") or {}).items():
                if _has_tag(module, reference.tag):
                    rank = next((index for index, tag in enumerate(module.get("tags") or [])
                                 if _tag_matches(str(tag), reference.tag)), 99)
                    rows.append((rank, plugin, model, module))
        rows.sort(key=lambda row: (row[0], row[1].casefold(), row[2].casefold()))
        out[reference.tag] = [(plugin, model, module)
                              for _, plugin, model, module in rows[:limit]]
    return out


def render_tag_context(references: list[TagReference], inv: dict) -> str:
    if not references:
        return ""
    rows = candidates(references, inv)
    out = ["\n---\n\n## VCV Library tag context\n"]
    for reference in references:
        intent = ("REQUIRED: include at least one of these in the final patch"
                  if reference.explicit else
                  "RETRIEVAL CUE: consider these when they fit the musical request")
        out.append(f"### {reference.tag}\n{intent}.")
        choices = rows.get(reference.tag) or []
        if not choices:
            out.append("No installed module currently carries this tag.")
            continue
        for plugin, model, module in choices:
            name = module.get("name") or model
            description = " ".join(str(module.get("description") or "").split())
            suffix = f" — {description[:100]}" if description else ""
            out.append(f"- `{plugin}/{model}` {name}{suffix}")
    return "\n".join(out) + "\n"


def required_tag_errors(patch: dict, inv: dict,
                        references: list[TagReference]) -> list[str]:
    """Verify explicit tag anchors against the patch, not model prose."""
    errors = []
    for reference in references:
        if not reference.explicit:
            continue
        matched = any(
            _has_tag((inv.get(module.get("plugin"), {}).get("modules", {})
                      .get(module.get("model"), {})), reference.tag)
            for module in patch.get("modules") or [])
        if not matched:
            errors.append(
                f"required #{reference.tag} tag has no matching module in the patch")
    return errors


def exclusive_maker_errors(patch: dict, mentions: dict) -> list[str]:
    """Keep an explicit "only @maker" promise closed, except Rack Core I/O."""
    exclusive = {brand: state for brand, state in mentions.items()
                 if state.get("exclusive")}
    if not exclusive:
        return []
    allowed = {slug for state in exclusive.values() for slug in state.get("slugs", [])}
    errors = []
    used = False
    for module in patch.get("modules") or []:
        plugin, model = module.get("plugin"), module.get("model")
        if plugin == "Core":
            continue
        if plugin in allowed:
            used = True
            continue
        errors.append(
            "exclusive maker constraint forbids "
            f"{plugin}/{model}; allowed makers are {', '.join(exclusive)}")
    if not used:
        errors.append("exclusive maker constraint requires at least one module from " +
                      ", ".join(exclusive))
    return errors
