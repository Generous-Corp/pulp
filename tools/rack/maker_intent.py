"""Deterministic maker-sourcing intent for Forge Modular prompts.

This module deliberately uses only the Python standard library.  It resolves
catalogue identities first, then interprets the small grammatical surface that
can change what Forge downloads, offers to a model, or admits in a patch.  It
is not a general English parser: unknown constructions remain UNDECIDED.
"""
from __future__ import annotations

from dataclasses import dataclass, replace
from enum import Enum
import re


class Decision(str, Enum):
    AFFIRMED = "AFFIRMED"
    EXCLUDED = "EXCLUDED"
    UNDECIDED = "UNDECIDED"


@dataclass(frozen=True)
class Evidence:
    start: int
    end: int
    text: str
    channel: str
    decision: Decision
    rule_id: str
    exclusive: bool = False
    exhaustive: bool = False


@dataclass(frozen=True)
class MakerResult:
    brand: str
    slugs: tuple[str, ...]
    decision: Decision
    rule_id: str
    exclusive: bool
    exhaustive: bool
    evidence: tuple[Evidence, ...]


@dataclass(frozen=True)
class Resolution:
    makers: tuple[MakerResult, ...]
    restrictive_ambiguities: tuple[str, ...] = ()

    def for_brand(self, brand: str) -> MakerResult | None:
        folded = fold_name(brand)
        return next((maker for maker in self.makers
                     if fold_name(maker.brand) == folded), None)

    def for_slug(self, slug: str) -> MakerResult | None:
        return next((maker for maker in self.makers if slug in maker.slugs), None)


def fold_name(text: str) -> str:
    return "".join(character for character in text.casefold()
                   if character.isalnum())


def _directory(catalogue: dict) -> tuple[
        tuple[str, str, tuple[str, ...], str], ...]:
    grouped: dict[str, tuple[str, list[str]]] = {}
    for slug, entry in catalogue.items():
        if not isinstance(entry, dict):
            continue
        brand = str(entry.get("brand") or entry.get("name") or slug)
        key = fold_name(brand)
        if not key:
            continue
        grouped.setdefault(key, (brand, []))[1].append(str(slug))
    identities = []
    for brand_key, (brand, slugs) in grouped.items():
        spellings = (brand, *slugs)
        seen = set()
        for spelling in spellings:
            key = fold_name(spelling)
            if not key or (key, spelling.casefold()) in seen:
                continue
            seen.add((key, spelling.casefold()))
            identities.append((key, brand, tuple(slugs), spelling))
    return tuple(sorted(identities, key=lambda item: -len(item[0])))


def _folded_source(text: str) -> tuple[str, tuple[int, ...]]:
    folded, offsets = [], []
    for index, character in enumerate(text):
        if character.isalnum():
            folded.append(character.casefold())
            offsets.extend([index] * len(character.casefold()))
    return "".join(folded), tuple(offsets)


def _spelling_parts(text: str) -> tuple[tuple[bool, str], ...]:
    """Alternating alphanumeric words and their literal separators."""
    parts: list[tuple[bool, str]] = []
    for character in text:
        alphanumeric = character.isalnum()
        if parts and parts[-1][0] == alphanumeric:
            kind, value = parts[-1]
            parts[-1] = (kind, value + character)
        else:
            parts.append((alphanumeric, character))
    return tuple(parts)


def _spelling_matches(candidate: str, brand: str) -> bool:
    """Allow case/spacing variation without inventing punctuation semantics."""
    candidate_parts = _spelling_parts(candidate)
    brand_parts = _spelling_parts(brand)
    if len(candidate_parts) != len(brand_parts):
        return False
    for (candidate_word, candidate_part), (brand_word, brand_part) in zip(
            candidate_parts, brand_parts):
        if candidate_word != brand_word:
            return False
        if candidate_word:
            if candidate_part.casefold() != brand_part.casefold():
                return False
        elif brand_part.isspace():
            # Documented word-separator variants are deliberately bounded to
            # one hyphen or underscore.  A colon, comma, parenthesis or a run
            # such as `---` expresses structure, not spelling.
            if not (candidate_part.isspace() or candidate_part in {"-", "_"}):
                return False
        else:
            # Whitespace around catalogue punctuation is presentation; the
            # punctuation itself is identity and must agree exactly.
            candidate_marks = "".join(
                character for character in candidate_part
                if not character.isspace())
            brand_marks = "".join(
                character for character in brand_part
                if not character.isspace())
            if candidate_marks != brand_marks:
                return False
    return True


def _identity_hits(prompt: str, catalogue: dict) -> list[dict]:
    folded, offsets = _folded_source(prompt)
    hits = []
    occupied: list[tuple[int, int]] = []
    for key, brand, slugs, spelling in _directory(catalogue):
        position = 0
        while True:
            index = folded.find(key, position)
            if index < 0:
                break
            position = index + 1
            start = offsets[index]
            end = offsets[index + len(key) - 1] + 1
            trailing_start = len(spelling)
            while (trailing_start and
                   not spelling[trailing_start - 1].isalnum()):
                trailing_start -= 1
            trailing = spelling[trailing_start:]
            # Folded offsets end at the final alphanumeric character. Preserve
            # catalogue-owned trailing punctuation such as the `)` in
            # Mathematics and Music Lab (MML).
            if trailing and prompt.startswith(trailing, end):
                end += len(trailing)
            candidate = prompt[start:end]
            if not _spelling_matches(candidate, spelling):
                continue
            before = prompt[start - 1] if start else ""
            after = prompt[end] if end < len(prompt) else ""
            if ((before.isalnum() and before != "@") or after.isalnum() or
                    any(start < used_end and used_start < end
                        for used_start, used_end in occupied)):
                continue
            explicit = before == "@"
            # Plugin/Module is the exact module channel with or without @. A
            # slash inside the catalogue maker spelling was consumed by hit.
            if after == "/":
                continue
            occupied.append((start, end))
            hits.append({"start": start, "end": end, "brand": brand,
                         "slugs": slugs, "explicit": explicit,
                         "text": prompt[start:end]})
    return sorted(hits, key=lambda hit: hit["start"])


_SOURCE = re.compile(
    r"\b(?:use|using|include|including|select|choose|prefer|draw|source|"
    r"build|make|create|want|from|by|with)\b", re.I)
_NEGATIVE = re.compile(
    r"\b(?:avoid|avoiding|exclude|excluding|skip|skipping|omit|omitting|"
    r"without|never|neither|nor|instead\s+of|leave(?:\s+\w+){0,3}\s+out|"
    r"rather\s+than|"
    r"do\s+not|does\s+not|did\s+not|don['’]t|doesn['’]t|didn['’]t|"
    r"not\s+from|no(?:\s+\w+){0,3}\s+from)\b", re.I)
_POST_NEGATIVE = re.compile(
    r"^\s*(?:(?:is|are)\s+not\s+(?:allowed|permitted|used|included|selected|"
    r"chosen)|(?:isn['’]t|aren['’]t)\s+(?:allowed|permitted|used|included|"
    r"selected|chosen)|(?:should|must|may|can)\s+not\s+(?:be\s+)?(?:allowed|"
    r"permitted|used|included|selected|chosen)|(?:cannot|can['’]t)\s+"
    r"(?:be\s+)?(?:allowed|permitted|used|included|selected|chosen)|"
    r"(?:should|must|may)\s+be\s+(?:avoided|excluded|forbidden|disallowed)|"
    r"(?:(?:is|are)\s+)?(?:forbidden|excluded|disallowed)\b|[- ]free\b|"
    r"\?\s*(?:no|not\s+allowed)|out\b)", re.I)
_POST_AFFIRMATIVE = re.compile(
    r"^\s*(?:is|are|should|must|may|can)\s+(?:be\s+)?"
    r"(?:allowed|permitted|used|included|selected|chosen)\b", re.I)
_RELATIVE = re.compile(r"\b(?:that|which|who|whose|where)\b", re.I)
_PARTICIPLE = re.compile(r"\b(?:[A-Za-z]+(?:ing|ed)|made|built|meant)\b", re.I)
_AUXILIARY = re.compile(
    r"\b(?:am|is|are|was|were|be|been|being|do|does|did|has|have|had|can|"
    r"could|may|might|must|shall|should|will|would)\b", re.I)
_QUANTIFIED_EXCEPTION = re.compile(
    r"\b(?:anything|everything|all|every(?:thing)?)\b", re.I)
_NOTHING_EXCEPTION = re.compile(r"\bnothing\b", re.I)
_EXCLUSIVE = re.compile(r"\b(?:only|just|exclusively|solely|purely)\b", re.I)
_EXHAUSTIVE = re.compile(r"\b(?:all|every|everything|entire|complete|whole)\b",
                         re.I)


def _sentence_start(prompt: str, position: int) -> int:
    stack: list[tuple[str, int]] = []
    pairs = {")": "(", "]": "[", "}": "{"}
    boundaries: list[tuple[int, int]] = []
    for index, character in enumerate(prompt[:position]):
        if character in "([{":
            stack.append((character, index))
        elif character in pairs and stack and stack[-1][0] == pairs[character]:
            stack.pop()
        elif character in ".;!?\n":
            boundaries.append((len(stack), index))
    target_depth = len(stack)
    last = next((index for depth, index in reversed(boundaries)
                 if depth == target_depth), -1)
    return last + 1


def _sentence_end(prompt: str, position: int) -> int:
    pairs = {")": "(", "]": "[", "}": "{"}
    stack: list[str] = []
    for character in prompt[:position]:
        if character in "([{":
            stack.append(character)
        elif character in pairs and stack and stack[-1] == pairs[character]:
            stack.pop()
    target_depth = len(stack)
    for index, character in enumerate(prompt[position:], start=position):
        if character in ".;!?\n" and len(stack) == target_depth:
            return index
        if character in "([{":
            stack.append(character)
        elif character in pairs and stack and stack[-1] == pairs[character]:
            stack.pop()
            if len(stack) < target_depth:
                return index
    return len(prompt)


def _top_level_matches(pattern: str, text: str) -> list[re.Match]:
    stack: list[str] = []
    pairs = {")": "(", "]": "[", "}": "{"}
    depths = []
    for character in text:
        depths.append(len(stack))
        if character in "([{":
            stack.append(character)
        elif character in pairs and stack and stack[-1] == pairs[character]:
            stack.pop()
    return [match for match in re.finditer(pattern, text, re.I)
            if match.start() >= len(depths) or depths[match.start()] == 0]


def _exception_decision(prefix: str) -> tuple[Decision, str] | None:
    markers = _top_level_matches(
        r"\b(?:but|except|other\s+than)\b", prefix)
    if not markers:
        return None
    marker = markers[-1]
    left, right = prefix[:marker.start()], prefix[marker.end():]
    if _NOTHING_EXCEPTION.search(left):
        return Decision.AFFIRMED, "exception.nothing_except"
    if not _QUANTIFIED_EXCEPTION.search(left):
        if marker.group(0).casefold() in {"except", "other than"}:
            return Decision.EXCLUDED, "exception.plain"
        return None

    source = list(_SOURCE.finditer(right))
    if not source:
        return Decision.EXCLUDED, "exception.quantified_phrase"
    predicate = source[-1]
    source_prefix = right[:predicate.start()]
    relative = _RELATIVE.search(source_prefix)
    participle = _PARTICIPLE.search(source_prefix)
    auxiliaries = list(_AUXILIARY.finditer(source_prefix))
    governed = bool(auxiliaries)
    if participle and auxiliaries and auxiliaries[-1].start() > participle.start():
        between = source_prefix[participle.end():auxiliaries[-1].start()]
        governed = not re.search(r"\b(?:so|that|which|who|whose|where)\b",
                                 between, re.I)
    if relative or (participle and not governed):
        return Decision.EXCLUDED, "exception.attached_phrase"
    return Decision.AFFIRMED, "exception.contrastive_clause"


def _whole_patch_exclusive(prefix: str, suffix: str) -> bool:
    raw = prefix[-140:]
    if re.search(r"\bonly\s+(?:one\s+maker|the\s+maker)\s*[:,]?\s*$",
                 raw, re.I):
        return True
    local = _local_governor(prefix)[-100:]
    if re.search(r"\bnot\s+only\s*$", local, re.I):
        return False
    if not _EXCLUSIVE.search(local) and not re.match(
            r"^\s*(?:modules?\s+)?(?:only|exclusively|solely|purely)\b",
            suffix, re.I):
        return False
    # These are quantity/role modifiers, not maker-wide source boundaries.
    suffix_clause = re.split(r"[.;!?]", suffix, maxsplit=1)[0]
    if re.search(r"\b(?:for|as)\s+(?!this\s+patch)", suffix_clause, re.I):
        return False
    if re.match(r"^\s+(?:only\s+)?(?:once|one\b)", suffix, re.I):
        return False
    if re.search(r"\bonly\s+(?:once\b|one\b|an?\b|the\b(?!\s+modules\b))",
                 local, re.I):
        return False
    if re.search(r"\bonly\s+(?:one|an?|the)\s+(?!modules?\b)\w+\s+"
                 r"(?:from|by)\s*$",
                 local, re.I):
        return False
    if re.search(r"\breplace\b[^.;!?]*\bwith\s+only\s*$", local, re.I):
        return False
    if re.search(r"\band\s+(?:add|include|use|choose|select)\b", suffix_clause,
                 re.I):
        return False
    return True


def _local_governor(prefix: str) -> str:
    """The coordinated clause that actually governs the following identity."""
    cuts = _top_level_matches(r"(?:,|\b(?:and|but|yet|however)\b)", prefix)
    for cut in reversed(cuts):
        right = prefix[cut.end():]
        if cut.group(0) == "," or _SOURCE.search(right) or re.search(
                r"\b(?:not|no|never|avoid|exclude|skip|omit|without|instead)\b",
                right, re.I):
            return right
    return prefix


def _maker_exhaustive(prefix: str) -> bool:
    """Whether all/every governs this maker or its sourced module set."""
    local = _local_governor(prefix)
    return bool(re.search(
        r"\b(?:all|every)\s+(?:of\s+)?(?:the\s+)?(?:modules?|plugins?)\s+"
        r"(?:from|by)\s*$|"
        r"\b(?:use|using|include|including|select|choose|source|draw)\s+"
        r"(?:all|every)\s+(?:(?:of\s+)?(?:the\s+)?(?:modules?|plugins?)\s+)?"
        r"(?:from|by)?\s*$",
        local, re.I))


def _postpositive_exclusive(prompt: str, hit: dict, hits: list[dict],
                            sentence_end: int) -> bool:
    """Accept `X [and Y] only`, never `X with/then add only one role`."""
    suffix = prompt[hit["end"]:sentence_end]
    marker = _EXCLUSIVE.search(suffix)
    if not marker:
        return False
    after = suffix[marker.end():]
    if re.match(r"\s+(?:once\b|one\b|an?\b|the\b)", after, re.I):
        return False
    marker_start = hit["end"] + marker.start()
    gaps = []
    cursor = hit["end"]
    for later in hits:
        if later["start"] < cursor or later["end"] > marker_start:
            continue
        gaps.append(prompt[cursor:later["start"]])
        cursor = later["end"]
    gaps.append(prompt[cursor:marker_start])
    residue = "".join(gaps)
    return bool(re.fullmatch(
        r"[\s,]*(?:(?:and|or|plus|nor|&|\+)[\s,]*)*"
        r"(?:modules?\s*)?"
        r"(?:(?:is|are|should|must|may|can|will|would)\s+"
        r"(?:be\s+)?(?:used|included|selected|chosen)\s*)?",
        residue, re.I))


def _negative_governs(prefix: str) -> bool:
    return bool(re.search(
        r"\b(?:avoid|exclude|skip|omit)(?:\W+\w+){0,6}\W*$|"
        r"\bleave(?:\W+\w+){0,5}\W+out\W*$|"
        r"\bwithout(?:\W+\w+){0,6}\W*$|"
        r"\bnever(?:\W+\w+){0,6}\W*$|"
        r"\bnot(?!\s+only\b)(?:\W+\w+){0,6}\W*$|"
        r"\bno(?:\W+\w+){0,6}\W*$|"
        r"\binstead\W+of\W*$|\bover\W*$|"
        r"\brather\W+than\W*$|"
        r"\bprefer(?:\W+\w+){1,12}\W+to\W*$|"
        r"\bneither(?:\W+\w+){0,5}\W*$|\bnor\W*$",
        prefix, re.I))


def _classify(prompt: str, hit: dict, previous: tuple[dict, Evidence] | None) -> Evidence:
    start, end = hit["start"], hit["end"]
    sentence = _sentence_start(prompt, start)
    prefix = prompt[sentence:start]
    suffix = prompt[end:]
    channel = "exact" if hit["explicit"] else "prose"
    inherited_evidence = None

    if _POST_NEGATIVE.match(suffix):
        decision, rule = Decision.EXCLUDED, "polarity.post_exclusion"
    elif _POST_AFFIRMATIVE.match(suffix):
        decision, rule = Decision.AFFIRMED, "polarity.post_affirmation"
    else:
        exceptional = _exception_decision(prefix)
        if exceptional:
            decision, rule = exceptional
        else:
            between = prompt[previous[0]["end"]:start] if previous else ""
            inherited = bool(previous and re.fullmatch(
                r"[\s,]*(?:(?:and|or|plus|nor|&|\+)[\s,]*)*", between, re.I))
            # A negative governor followed only by a coordinated noun phrase
            # applies to every maker in the phrase.
            if inherited:
                decision = previous[1].decision
                rule = "coordination.inherit"
                inherited_evidence = previous[1]
            else:
                local_prefix = _local_governor(prefix)
                negatives = list(_NEGATIVE.finditer(local_prefix))
                sources = list(_SOURCE.finditer(local_prefix))
                last_negative = negatives[-1] if negatives else None
                last_source = sources[-1] if sources else None
                double_negative = bool(re.search(
                    r"\b(?:do\s+not|don['’]t)\s+(?:want\s+to\s+)?"
                    r"(?:avoid|exclude|omit|skip|dislike|hate)\b",
                    local_prefix, re.I))
                not_without = bool(re.search(
                    r"\bnot\s+without(?:\W+\w+){0,5}\W*$",
                    local_prefix, re.I))
                if not_without:
                    # Logically this can sound affirmative, but it is not a
                    # sourcing instruction.  Treating it as one made an
                    # uncertain permission silently download a maker.
                    decision, rule = Decision.UNDECIDED, "polarity.not_without"
                elif double_negative:
                    decision, rule = Decision.UNDECIDED, "polarity.permission"
                elif ("?" in suffix and re.search(
                        r"\b(?:should|can|could|would|may)\s+(?:we|I)\b",
                        local_prefix, re.I)):
                    decision, rule = Decision.UNDECIDED, "polarity.question"
                elif _negative_governs(local_prefix) or (
                        last_negative and (not last_source or
                                        last_negative.start() >= last_source.start() or
                                        re.search(r"\b(?:do\s+not|don['’]t)\s+"
                                                  r"(?:want\s+to\s+)?(?:use|include|"
                                                  r"choose|select|prefer)?[^,;.!?]*$",
                                                  local_prefix, re.I))):
                    decision, rule = Decision.EXCLUDED, "polarity.negative_governor"
                elif (last_source and
                      not (len(fold_name(hit["brand"])) < 6 and
                           last_source.group(0).casefold() == "with" and
                           re.search(r"\b(?:a|an|the)\b",
                                     local_prefix[last_source.end():], re.I))):
                    decision, rule = Decision.AFFIRMED, "polarity.sourcing_predicate"
                elif (len(fold_name(hit["brand"])) < 6 and
                      _whole_patch_exclusive(prefix, suffix) and
                      (hit["explicit"] or hit["text"] == hit["brand"])):
                    # Exact-case short brands are normally too ambiguous in
                    # prose (`as` is an ordinary word). An explicit exclusive
                    # grammar cue makes `only AS` an intentional identity.
                    decision, rule = Decision.AFFIRMED, "polarity.exclusive_identity"
                elif hit["explicit"]:
                    decision, rule = Decision.AFFIRMED, "identity.exact_default"
                elif (hit["text"][:1].isupper() and
                      len(fold_name(hit["brand"])) >= 6 and not re.match(
                        r"^\s+(?:is|are|was|were)\s+", suffix, re.I)):
                    decision, rule = Decision.AFFIRMED, "identity.distinctive_bare"
                else:
                    decision, rule = Decision.UNDECIDED, "polarity.no_sourcing_act"

    exclusive = decision == Decision.AFFIRMED and (
        _exception_decision(prefix) ==
        (Decision.AFFIRMED, "exception.nothing_except") or
        _whole_patch_exclusive(prefix, suffix) or
        bool(inherited_evidence and inherited_evidence.exclusive))
    # Qualifiers belong to the phrase governing this maker, not arbitrary prose
    # nearby.  Without the clause cut, `connect all cables, then use X` made X
    # exhaustive merely because `all` happened to be within eighty characters.
    exhaustive = (decision == Decision.AFFIRMED and
                  _maker_exhaustive(prefix) and
                  not bool(re.search(r"\b(?:but|except)\b[^.;!?]*$", prefix,
                                     re.I))) or bool(
        decision == Decision.AFFIRMED and inherited_evidence and
        inherited_evidence.exhaustive)
    return Evidence(start, end, hit["text"], channel, decision, rule,
                    exclusive, exhaustive)


def resolve(prompt: str, catalogue: dict) -> Resolution:
    """Resolve all catalogue maker mentions in source order."""
    hits = _identity_hits(prompt, catalogue)
    evidence_by_brand: dict[str, list[Evidence]] = {}
    meta: dict[str, tuple[str, ...]] = {}
    previous = None
    for hit in hits:
        evidence = _classify(prompt, hit, previous)
        evidence_by_brand.setdefault(hit["brand"], []).append(evidence)
        meta[hit["brand"]] = hit["slugs"]
        previous = (hit, evidence)

    # A postpositive predicate governs the coordinated noun phrase before it:
    # `X and Y are forbidden/should be used` is one assertion, not two
    # unrelated maker decisions.
    for index, hit in enumerate(hits):
        current_occurrence = sum(
            1 for earlier in hits[:index] if earlier["brand"] == hit["brand"])
        current = evidence_by_brand[hit["brand"]][current_occurrence]
        if current.rule_id not in {"polarity.post_exclusion",
                                   "polarity.post_affirmation"}:
            continue
        cursor = index - 1
        while cursor >= 0 and re.fullmatch(
                r"[\s,]*(?:(?:and|or|plus|nor|&|\+)[\s,]*)*",
                prompt[hits[cursor]["end"]:hits[cursor + 1]["start"]], re.I):
            prior_hit = hits[cursor]
            occurrence = sum(
                1 for earlier in hits[:cursor]
                if earlier["brand"] == prior_hit["brand"])
            prior = evidence_by_brand[prior_hit["brand"]][occurrence]
            evidence_by_brand[prior_hit["brand"]][occurrence] = replace(
                prior, decision=current.decision,
                rule_id="coordination.post_inherit")
            cursor -= 1

    # Qualifiers are applied only after postpositive predicate inheritance, so
    # a short initially-undecided brand such as AS receives both AFFIRMED and
    # the list-wide qualifier from `AS and Bogaudio should be used exclusively`.
    for index, hit in enumerate(hits):
        brand = hit["brand"]
        item = evidence_by_brand[brand][sum(
            1 for earlier in hits[:index] if earlier["brand"] == brand)]
        if item.decision != Decision.AFFIRMED:
            continue
        sentence_end = _sentence_end(prompt, hit["end"])
        clause = prompt[_sentence_start(prompt, hit["start"]):sentence_end]
        scoped = bool(re.search(r"\bfor\s+(?!this\s+patch)\w+", clause, re.I) or
                      re.search(r"\band\s+(?:add|include|use|choose|select)\b",
                                clause, re.I))
        post_only = _postpositive_exclusive(prompt, hit, hits, sentence_end)
        if scoped and item.exclusive:
            replacement = replace(item, exclusive=False)
        elif post_only and not scoped and not item.exclusive:
            replacement = replace(item, exclusive=True)
        else:
            continue
        occurrence = evidence_by_brand[brand].index(item)
        evidence_by_brand[brand][occurrence] = replacement

    results = []
    ambiguities = []
    for brand, evidence in evidence_by_brand.items():
        if any(item.decision == Decision.EXCLUDED for item in evidence):
            decision, rule = Decision.EXCLUDED, "aggregate.negative_veto"
        elif any(item.decision == Decision.AFFIRMED for item in evidence):
            decision, rule = Decision.AFFIRMED, "aggregate.affirmed"
        else:
            decision, rule = Decision.UNDECIDED, "aggregate.undecided"
        exclusive = decision == Decision.AFFIRMED and any(
            item.exclusive for item in evidence)
        exhaustive = decision == Decision.AFFIRMED and any(
            item.exhaustive for item in evidence)
        if decision == Decision.UNDECIDED and any(
                _EXCLUSIVE.search(prompt[
                    _sentence_start(prompt, item.start):
                    _sentence_end(prompt, item.end)])
                for item in evidence):
            ambiguities.append(
                f"restrictive maker request for {brand} is ambiguous")
        results.append(MakerResult(brand, meta[brand], decision, rule,
                                   exclusive, exhaustive, tuple(evidence)))
    return Resolution(tuple(results), tuple(ambiguities))


def affirmed(resolution: Resolution) -> tuple[MakerResult, ...]:
    return tuple(maker for maker in resolution.makers
                 if maker.decision == Decision.AFFIRMED)


def excluded(resolution: Resolution) -> tuple[MakerResult, ...]:
    return tuple(maker for maker in resolution.makers
                 if maker.decision == Decision.EXCLUDED)
