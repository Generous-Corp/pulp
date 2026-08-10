#!/usr/bin/env python3
"""Recover text from PDFs whose embedded subset fonts lack ToUnicode maps.

The output is machine-local source material: write it under the external Rack
corpus, never into this repository.  The recovery code is generic and accepts
its language-model texts explicitly; it knows nothing about a particular book
or a particular user's corpus layout.

    python3 recover_subset_font_pdf.py broken.pdf recovered.txt \
        --model-text readable-book.txt --model-text readable-corpus/

Each page/font pair is treated as its own substitution alphabet.  Uniform
offset encodings are tried first.  Arbitrary substitutions are attacked with
word-pattern constraints and quadgram hill climbing, retaining a block only
when enough of its characters participate in known words.  Page markers and a
summary make both provenance and the unrecovered denominator explicit.
"""
from __future__ import annotations

import argparse
import collections
import dataclasses
import html
import math
import os
import random
import re
import stat
import subprocess
import sys
import tempfile
from collections.abc import Iterable

TEXT_SUFFIXES = (".md", ".sc", ".txt")
ALPHA = "abcdefghijklmnopqrstuvwxyz"
FREQUENCY_ORDER = "etaoinshrdlcumfpgwybvkxjqz"
HERE = os.path.dirname(os.path.abspath(__file__))
REPOSITORY = os.path.realpath(os.path.join(HERE, "..", ".."))


def external_output_path(path: str) -> str:
    """Resolve an output path and reject destinations inside the repository."""
    # realpath resolves both an existing output-file symlink and every existing
    # parent symlink. A lexical /tmp path can otherwise land inside the checkout.
    destination = os.path.realpath(os.path.expanduser(path))
    try:
        inside_repository = os.path.commonpath(
            (destination, REPOSITORY)) == REPOSITORY
    except ValueError:
        inside_repository = False
    if inside_repository:
        raise ValueError("recovered source output must be outside the repository")
    return destination


def write_external_output(path: str, text: str) -> str:
    """Write outside the checkout without following a swapped path component."""
    destination = external_output_path(path)
    parent = os.path.dirname(destination)
    os.makedirs(parent, exist_ok=True)
    destination = external_output_path(destination)
    parent = os.path.dirname(destination)
    parent_flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    parent_flags |= getattr(os, "O_NOFOLLOW", 0)
    parent_fd = os.open(parent, parent_flags)
    try:
        # Resolve the already-open directory, not the pathname checked before
        # open. If an attacker swapped the parent between those operations,
        # this is the directory we would actually write through.
        fd_link = (f"/dev/fd/{parent_fd}" if os.path.exists("/dev/fd")
                   else f"/proc/self/fd/{parent_fd}")
        opened_parent = external_output_path(os.path.realpath(fd_link))
        if os.path.basename(destination) in ("", ".", ".."):
            raise ValueError("recovery output must name a regular file")
        flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
        flags |= getattr(os, "O_NOFOLLOW", 0)
        output_fd = os.open(os.path.basename(destination), flags, 0o600,
                            dir_fd=parent_fd)
        try:
            if not stat.S_ISREG(os.fstat(output_fd).st_mode):
                raise ValueError("recovery output must be a regular file")
            # Keep the checked open-parent value live and explicit: the file
            # descriptor is relative to this directory, not to the old path.
            if not opened_parent:
                raise ValueError("could not resolve opened output directory")
            with os.fdopen(output_fd, "w", encoding="utf-8") as output:
                output_fd = -1
                output.write(text)
        finally:
            if output_fd >= 0:
                os.close(output_fd)
    finally:
        os.close(parent_fd)
    return destination


def pattern(word: str) -> tuple[int, ...]:
    """The repeated-symbol shape of a word (e.g. paper -> 0,1,0,2,3)."""
    seen: dict[str, int] = {}
    out = []
    for char in word:
        if char not in seen:
            seen[char] = len(seen)
        out.append(seen[char])
    return tuple(out)


@dataclasses.dataclass(frozen=True)
class LanguageModel:
    vocabulary: frozenset[str]
    quadgrams: dict[str, float]
    floor: float


def _model_files(paths: Iterable[str]) -> list[str]:
    files = []
    for path in paths:
        path = os.path.abspath(os.path.expanduser(path))
        if os.path.isdir(path):
            for root, dirs, names in os.walk(path):
                dirs[:] = sorted(d for d in dirs if d != ".git")
                files.extend(os.path.join(root, name) for name in sorted(names)
                             if name.lower().endswith(TEXT_SUFFIXES))
        elif os.path.isfile(path):
            files.append(path)
        else:
            raise ValueError(f"language-model input does not exist: {path}")
    return sorted(set(files))


def build_language_model(paths: Iterable[str]) -> LanguageModel:
    """Build an offline word/quadgram model from explicitly supplied texts."""
    blobs = []
    for path in _model_files(paths):
        with open(path, encoding="utf-8", errors="replace") as source:
            blobs.append(source.read())
    blob = "\n".join(blobs)
    if len(blob) < 10_000:
        raise ValueError("language-model inputs contain too little text "
                         f"({len(blob):,} characters; need at least 10,000)")

    words = collections.Counter(
        word.lower() for word in re.findall(r"[A-Za-z]{2,}", blob))
    vocabulary = frozenset(word for word, count in words.items() if count >= 2)
    letters = re.sub(r"[^A-Z]", "", blob.upper())
    counts = collections.Counter(letters[i:i + 4]
                                 for i in range(len(letters) - 3))
    total = sum(counts.values())
    if not vocabulary or not total:
        raise ValueError("language-model inputs contain no usable English")
    floor = math.log10(0.01 / total)
    quadgrams = {gram: math.log10(count / total)
                 for gram, count in counts.items()}
    return LanguageModel(vocabulary, quadgrams, floor)


def word_score(text: str, vocabulary: frozenset[str]) -> float:
    """Share of all non-space characters participating in known words."""
    body = "".join(text.split())
    if len(body) < 20:
        return 0.0
    hit = sum(len(token) for token in re.findall(r"[A-Za-z]{2,}", text)
              if token.lower() in vocabulary)
    return hit / len(body)


def quad_score(text: str, model: LanguageModel) -> float:
    letters = re.sub(r"[^A-Z]", "", text.upper())
    if len(letters) < 4:
        return -1e9
    return sum(model.quadgrams.get(letters[i:i + 4], model.floor)
               for i in range(len(letters) - 3)) / (len(letters) - 3)


def apply_offset(cipher: str, offset: int) -> str:
    return "".join(chr((ord(char) + offset) % 256) if ord(char) < 256 else char
                   for char in cipher)


def best_offset(cipher: str, model: LanguageModel) -> tuple[float, int, str]:
    best = (-1.0, 0, cipher)
    # Zero matters: some fonts in an otherwise broken PDF extract correctly,
    # and transforming known-good text must never make it worse.
    for offset in range(256):
        decoded = apply_offset(cipher, offset)
        candidate = (word_score(decoded, model.vocabulary), offset, decoded)
        if candidate[0] > best[0]:
            best = candidate
    return best


def split_words(cipher: str, space: str) -> list[str]:
    return [chunk for chunk in re.split(r"\s+", cipher.replace(space, " "))
            if 4 <= len(chunk) <= 18]


class PatternSolver:
    """Monoalphabetic solver using word repetition-pattern constraints."""

    def __init__(self, vocabulary: frozenset[str], min_length: int = 4):
        self.by_pattern: dict[tuple[int, ...], list[str]] = \
            collections.defaultdict(list)
        for word in sorted(vocabulary):
            if min_length <= len(word) <= 18 and word.isalpha():
                self.by_pattern[pattern(word)].append(word)

    def solve(self, words: list[str], budget: int = 250_000) -> dict[str, str]:
        frequencies = collections.Counter(words)
        candidates = []
        for word, count in frequencies.items():
            options = self.by_pattern.get(pattern(word), [])
            if options and len(options) <= 500:
                candidates.append((len(options), -len(word), -count,
                                   word, options))
        candidates.sort(key=lambda row: row[:4])
        candidates = candidates[:70]
        if len(candidates) < 5:
            return {}

        key: dict[str, str] = {}
        reverse: dict[str, str] = {}
        best: tuple[int, int, dict[str, str]] = (0, 0, {})
        steps = 0

        def assign(cipher_word: str, plain_word: str) -> list[str] | None:
            added = []
            for cipher_char, plain_char in zip(cipher_word, plain_word):
                if cipher_char in key:
                    if key[cipher_char] != plain_char:
                        undo(added)
                        return None
                elif plain_char in reverse:
                    undo(added)
                    return None
                else:
                    key[cipher_char] = plain_char
                    reverse[plain_char] = cipher_char
                    added.append(cipher_char)
            return added

        def undo(added: list[str]) -> None:
            for cipher_char in reversed(added):
                plain_char = key.pop(cipher_char)
                reverse.pop(plain_char)

        def visit(index: int, placed: int, covered: int) -> None:
            nonlocal best, steps
            steps += 1
            if steps > budget:
                return
            rank = (covered, placed)
            if rank > best[:2]:
                best = (covered, placed, dict(key))
            if index >= len(candidates):
                return
            _, neg_length, neg_count, cipher_word, options = candidates[index]
            for plain_word in options:
                added = assign(cipher_word, plain_word)
                if added is None:
                    continue
                visit(index + 1, placed + 1,
                      covered + (-neg_length * -neg_count))
                undo(added)
                if steps > budget:
                    return
            # OCR debris or domain words need not be in the model vocabulary.
            visit(index + 1, placed, covered)

        visit(0, 0, 0)
        return best[2]


def decode_with(key: dict[str, str], cipher: str, space: str) -> str:
    return "".join(" " if char == space else key.get(char, "·")
                   if not char.isspace() else " " for char in cipher)


def complete_key(key: dict[str, str], cipher: str, space: str,
                 model: LanguageModel, limit: int = 10) -> dict[str, str]:
    """Greedily fill high-frequency symbols left unknown by constraints."""
    frequencies = collections.Counter(
        char for char in cipher if not char.isspace() and char != space)
    unknown = [char for char, _ in frequencies.most_common(32)
               if char not in key][:limit]
    for symbol in unknown:
        used = set(key.values())
        best: tuple[float, str] | None = None
        for letter in ALPHA:
            if letter in used:
                continue
            key[symbol] = letter
            decoded = decode_with(key, cipher, space)
            score = word_score(decoded, model.vocabulary) + \
                0.025 * quad_score(decoded, model)
            candidate = (score, letter)
            if best is None or candidate > best:
                best = candidate
            key.pop(symbol)
        if best:
            key[symbol] = best[1]
    return key


def hill_climb(cipher: str, space: str, model: LanguageModel,
               rounds: int = 10) -> tuple[float, str]:
    frequencies = collections.Counter(
        char for char in cipher if not char.isspace() and char != space)
    symbols = [char for char, _ in frequencies.most_common(26)]
    if len(symbols) < 16:
        return 0.0, ""
    key = {symbol: FREQUENCY_ORDER[index]
           for index, symbol in enumerate(symbols)}

    def decoded(mapping: dict[str, str]) -> str:
        return decode_with(mapping, cipher, space)

    best_key = dict(key)
    best_quad = quad_score(decoded(best_key), model)
    rng = random.Random(0x5EED)
    for round_index in range(rounds):
        key = dict(best_key)
        improved = True
        while improved:
            improved = False
            for left in range(len(symbols)):
                for right in range(left + 1, len(symbols)):
                    a, b = symbols[left], symbols[right]
                    key[a], key[b] = key[b], key[a]
                    score = quad_score(decoded(key), model)
                    if score > best_quad:
                        best_quad, best_key = score, dict(key)
                        improved = True
                    else:
                        key[a], key[b] = key[b], key[a]
        key = dict(best_key)
        for _ in range(2 + round_index % 4):
            a, b = rng.sample(symbols, 2)
            key[a], key[b] = key[b], key[a]
    result = decoded(best_key)
    return word_score(result, model.vocabulary), result


@dataclasses.dataclass(frozen=True)
class DecodedBlock:
    text: str
    score: float
    method: str
    source_characters: int


def decode_block(cipher: str, model: LanguageModel,
                 offset_threshold: float = 0.35,
                 substitution_threshold: float = 0.30,
                 solver: PatternSolver | None = None,
                 space_candidates: int = 1) -> DecodedBlock | None:
    source_characters = len(re.sub(r"\s", "", cipher))
    score, offset, decoded = best_offset(cipher, model)
    if score >= offset_threshold:
        return DecodedBlock(decoded, score, f"offset:{offset}", source_characters)

    frequencies = collections.Counter(char for char in cipher
                                      if not char.isspace())
    if len(frequencies) < 16:
        return None
    solver = solver or PatternSolver(model.vocabulary)
    candidates: list[tuple[float, str, str]] = []
    # Space is usually first, but short captions and tables can make a letter
    # more frequent. Deep search may try the top three; the bounded default
    # tries one because a whole book contains hundreds of independent fonts.
    for space, _ in frequencies.most_common(space_candidates):
        key = solver.solve(split_words(cipher, space))
        if key:
            key = complete_key(dict(key), cipher, space, model)
            text = decode_with(key, cipher, space)
            candidates.append((word_score(text, model.vocabulary),
                               "pattern", text))
    # Pattern constraints are both sharper and much cheaper. Quadgram climbing
    # is a fallback for fonts whose prose has too many out-of-vocabulary words,
    # not three additional full searches after an answer already cleared the
    # admission threshold.
    if candidates and max(candidates, key=lambda row: row[0])[0] >= \
            substitution_threshold:
        score, method, decoded = max(candidates, key=lambda row: row[0])
        return DecodedBlock(decoded, score, method, source_characters)
    for space, _ in frequencies.most_common(space_candidates):
        score, text = hill_climb(cipher, space, model)
        if text:
            candidates.append((score, "quadgram", text))
    if not candidates:
        return None
    score, method, decoded = max(candidates, key=lambda row: row[0])
    if score < substitution_threshold:
        return None
    return DecodedBlock(decoded, score, method, source_characters)


def parse_xml(xml: str) -> list[tuple[int, dict[str, list[str]]]]:
    """Return page/font runs; tolerate the control bytes broken PDFs emit."""
    pages = []
    chunks = re.split(r'<page number="(\d+)"', xml)
    for index in range(1, len(chunks), 2):
        number, body = int(chunks[index]), chunks[index + 1]
        runs: dict[str, list[str]] = collections.defaultdict(list)
        for font, text in re.findall(
                r'<text[^>]*font="(\d+)"[^>]*>(.*?)</text>', body, re.S):
            clean = html.unescape(re.sub(r"<[^>]+>", "", text))
            runs[font].append(clean)
        pages.append((number, dict(runs)))
    return pages


@dataclasses.dataclass
class RecoveryStats:
    pages_total: int = 0
    pages_recovered: int = 0
    blocks_total: int = 0
    blocks_recovered: int = 0
    characters_total: int = 0
    characters_recovered: int = 0
    methods: collections.Counter = dataclasses.field(
        default_factory=collections.Counter)

    @property
    def coverage(self) -> float:
        return self.characters_recovered / max(1, self.characters_total)


def recover_xml(xml: str, model: LanguageModel, minimum_characters: int = 40,
                first_page: int | None = None,
                last_page: int | None = None,
                space_candidates: int = 1) -> tuple[str, RecoveryStats]:
    stats = RecoveryStats()
    output = []
    # The vocabulary index is book-invariant and takes meaningful time to
    # construct. Building it once per font block turned a recovery pass into
    # hundreds of identical indexing jobs.
    solver = PatternSolver(model.vocabulary)
    for number, runs in parse_xml(xml):
        if first_page is not None and number < first_page:
            continue
        if last_page is not None and number > last_page:
            continue
        stats.pages_total += 1
        recovered = []
        for font, texts in runs.items():
            cipher = "\n".join(texts)
            characters = len(re.sub(r"\s", "", cipher))
            if characters < minimum_characters:
                continue
            stats.blocks_total += 1
            stats.characters_total += characters
            block = decode_block(cipher, model, solver=solver,
                                 space_candidates=space_candidates)
            if not block:
                continue
            stats.blocks_recovered += 1
            stats.characters_recovered += characters
            stats.methods[block.method] += 1
            recovered.append((characters, font, block))
        if recovered:
            stats.pages_recovered += 1
            output.append(f"\n=== page {number} ===\n")
            for _, font, block in sorted(recovered, reverse=True):
                output.append(f"[{block.method} font={font} "
                              f"quality={block.score:.2f}]\n")
                text = re.sub(r"[ \t]+", " ", block.text).strip()
                output.append(text + "\n")
    return "\n".join(output), stats


def pdf_xml(path: str, first_page: int | None,
            last_page: int | None) -> str:
    if not os.path.isfile(path):
        raise ValueError(f"input PDF does not exist: {path}")
    with tempfile.TemporaryDirectory(prefix="pulp-subset-font-") as tmp:
        output = os.path.join(tmp, "source.xml")
        command = ["pdftohtml", "-q", "-xml", "-hidden", "-i"]
        if first_page is not None:
            command += ["-f", str(first_page)]
        if last_page is not None:
            command += ["-l", str(last_page)]
        command += [path, output]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode or not os.path.exists(output):
            detail = result.stderr.strip() or "no XML was produced"
            raise RuntimeError(f"pdftohtml failed: {detail}")
        with open(output, encoding="utf-8", errors="replace") as source:
            return source.read()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pdf")
    parser.add_argument("output")
    parser.add_argument("--model-text", action="append", required=True,
                        help="readable text file or directory; repeatable")
    parser.add_argument("--first-page", type=int)
    parser.add_argument("--last-page", type=int)
    parser.add_argument("--minimum-characters", type=int, default=40)
    parser.add_argument(
        "--deep-search", action="store_true",
        help="try three possible encoded-space glyphs per hard block; slower")
    args = parser.parse_args(argv[1:])
    if args.first_page and args.last_page and args.first_page > args.last_page:
        parser.error("--first-page must not exceed --last-page")

    try:
        model = build_language_model(args.model_text)
        xml = pdf_xml(args.pdf, args.first_page, args.last_page)
        recovered, stats = recover_xml(
            xml, model, args.minimum_characters,
            args.first_page, args.last_page,
            space_candidates=3 if args.deep_search else 1)
        destination = write_external_output(
            args.output,
            "# Machine-recovered subset-font text; imperfect.\n" + recovered)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"recovery failed: {error}", file=sys.stderr)
        return 2

    print(f"pages {stats.pages_recovered}/{stats.pages_total}; blocks "
          f"{stats.blocks_recovered}/{stats.blocks_total}; characters "
          f"{stats.characters_recovered}/{stats.characters_total} "
          f"({stats.coverage:.1%}); methods {dict(stats.methods)}")
    print(f"output: {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
