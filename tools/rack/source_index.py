#!/usr/bin/env python3
"""Build and query the machine-local Forge Modular reading index.

The index is a research aid, not a generation knowledge base.  It makes the
locally held corpus searchable so an agent can find passages worth turning
into reviewed technique candidates.  Only records admitted by ``knowledge.py``
may enter a generation prompt.

Source text and the SQLite database stay below Forge Modular's Application
Support corpus directory.  Neither is part of the repository or app payload.

    python3 source_index.py build
    python3 source_index.py query "sample and hold evolving random voltage"
    python3 source_index.py query "acid accent slide" --json
    python3 source_index.py status
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import re
import sqlite3
import stat
import sys
from collections.abc import Iterable


DEFAULT_CORPUS = os.environ.get(
    "PULP_RACK_CORPUS",
    os.path.expanduser("~/Library/Application Support/Forge Modular/.corpus"),
)
DEFAULT_DB_NAME = "source-index.sqlite3"
SCHEMA_VERSION = 1
INDEXER_VERSION = 4
TEXT_SUFFIXES = (".md", ".txt", ".sc")
MAX_RESULTS = 20


class SourceIndexError(RuntimeError):
    """The local index cannot be created or queried safely."""


@dataclasses.dataclass(frozen=True)
class Source:
    key: str
    work_id: str
    title: str
    kind: str
    extraction: str
    path: str
    sha256: str
    byte_size: int
    text: str


@dataclasses.dataclass(frozen=True)
class Chunk:
    ordinal: int
    locator: str
    page: int | None
    section: str
    concepts: str
    text: str
    sha256: str


@dataclasses.dataclass(frozen=True)
class BuildSummary:
    added: int
    updated: int
    unchanged: int
    removed: int
    sources: int
    chunks: int


# These are retrieval aliases, not claims about synthesis.  They let a query
# use a musician's word while the source uses a technical synonym.  The
# resulting passage is still only evidence to review, never admitted guidance.
CONCEPT_TERMS: dict[str, tuple[str, ...]] = {
    "acid": ("acid", "303", "tb-303", "slide", "accent"),
    "amplitude": ("amplitude", "gain", "level", "loudness", "vca", "accent"),
    "delay": ("delay", "echo", "comb", "flange", "chorus"),
    "envelope": ("envelope", "contour", "attack", "decay", "sustain", "release"),
    "feedback": ("feedback", "regeneration", "self-oscillation", "self oscillation"),
    "filtering": ("filter", "cutoff", "resonance", "low-pass", "high-pass",
                  "band-pass", "formant"),
    "frequency-modulation": ("frequency modulation", "fm", "index", "sideband"),
    "modulation": ("modulation", "modulator", "lfo", "vibrato", "tremolo"),
    "percussion": ("percussion", "drum", "kick", "snare", "cymbal", "marimba"),
    "pitch": ("pitch", "frequency", "tuning", "octave", "interval", "1v/oct"),
    "random": ("random", "sample and hold", "sample-and-hold", "stochastic",
               "uncertainty", "noise"),
    "ring-modulation": ("ring modulation", "ring modulator", "four quadrant"),
    "sequencing": ("sequencer", "sequence", "clock", "step", "rhythm",
                   "trigger", "pulse"),
    "slew": ("slew", "glide", "portamento", "lag", "slide"),
    "spatial": ("stereo", "spatial", "pan", "panning", "localization"),
    "synthesis": ("oscillator", "waveform", "synthesis", "harmonic", "timbre"),
}

_STOP_WORDS = {
    "a", "about", "an", "and", "are", "as", "at", "be", "build", "can",
    "create", "for", "from", "get", "how", "i", "in", "into", "is", "it",
    "make", "of", "on", "or", "patch", "sound", "that", "the", "this", "to",
    "use", "using", "want", "with",
}


def default_db_path(corpus: str) -> str:
    return os.path.join(corpus, DEFAULT_DB_NAME)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _slug(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")


def _source_identity(relative_path: str) -> tuple[str, str, str, str]:
    """Return stable work id, display title, kind, and extraction identity."""
    rel = relative_path.replace(os.sep, "/")
    stem = os.path.splitext(os.path.basename(rel))[0]
    extraction = "reading-order" if stem.endswith("-reading-order") else "source"
    canonical = re.sub(r"-reading-order$", "", stem)

    if rel.startswith("synth-secrets/"):
        return ("sound-on-sound-synth-secrets",
                "Sound On Sound: Synth Secrets", "fetched-series", extraction)
    if rel.startswith("cookbook-sc/"):
        return ("welsh-synthesizer-cookbook-foundations",
                "Welsh's Synthesizer Cookbook foundations", "fetched-notes",
                extraction)

    known = (
        ("strange-electronic-music-systems", "allen-strange-electronic-music-systems",
         "Allen Strange: Electronic Music Systems"),
        ("the-book-of-bad-ideas", "the-book-of-bad-ideas",
         "The Book of Bad Ideas"),
        ("patch-tweak", "patch-and-tweak", "Patch & Tweak"),
        ("welshs-synthesizer-cookbook", "welsh-synthesizer-cookbook",
         "Welsh's Synthesizer Cookbook"),
        ("roads-curtis-the-computer-music-tutorial", "roads-computer-music-tutorial",
         "Curtis Roads: The Computer Music Tutorial"),
        ("arp2600", "arp-2600", "ARP 2600 reference"),
        ("makenoise-maths", "make-noise-maths", "Make Noise MATHS manual"),
        ("makenoise-shared-system", "make-noise-shared-system",
         "Make Noise Shared System manual"),
    )
    for needle, work_id, title in known:
        if needle in canonical:
            kind = "local-book" if rel.startswith("local/") else "fetched-manual"
            return work_id, title, kind, extraction

    title = re.sub(r"[-_]", " ", canonical).strip().title()
    kind = "local-source" if rel.startswith("local/") else "fetched-source"
    return _slug(canonical), title, kind, extraction


def discover_sources(corpus: str) -> list[Source]:
    """Read each unique searchable text document below ``corpus``."""
    found: list[Source] = []
    if not os.path.isdir(corpus):
        raise SourceIndexError(
            f"no reading corpus at {corpus}; run tools/rack/corpus.py first"
        )
    for root, dirs, files in os.walk(corpus):
        dirs[:] = sorted(d for d in dirs if d != ".git")
        for name in sorted(files):
            if not name.lower().endswith(TEXT_SUFFIXES):
                continue
            path = os.path.join(root, name)
            try:
                with open(path, "rb") as source_file:
                    raw = source_file.read()
            except OSError as exc:
                raise SourceIndexError(f"cannot read corpus source {path}: {exc}") \
                    from exc
            rel = os.path.relpath(path, corpus).replace(os.sep, "/")
            work_id, title, kind, extraction = _source_identity(rel)
            found.append(Source(
                key=rel,
                work_id=work_id,
                title=title,
                kind=kind,
                extraction=extraction,
                path=os.path.abspath(path),
                sha256=_sha256(raw),
                byte_size=len(raw),
                text=raw.decode("utf-8", "replace"),
            ))
    return found


def _concepts(text: str) -> str:
    low = text.lower()
    matched = []
    for concept, terms in CONCEPT_TERMS.items():
        if any(re.search(r"(?<!\w)" + re.escape(term) + r"(?!\w)", low)
               for term in terms):
            matched.append(concept)
    return " ".join(matched)


def _split_long(text: str, maximum: int = 3600) -> Iterable[str]:
    rest = text.strip()
    while len(rest) > maximum:
        cut = rest.rfind(" ", 0, maximum)
        if cut < maximum // 2:
            cut = maximum
        yield rest[:cut].strip()
        rest = rest[cut:].strip()
    if rest:
        yield rest


def _page_blocks(text: str) -> Iterable[tuple[int | None, str]]:
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    recovered_pages = list(re.finditer(
        r"(?m)^===\s*page\s+(\d+)\s*===\s*$", text
    ))
    if recovered_pages:
        preamble = text[:recovered_pages[0].start()].strip()
        if preamble:
            yield None, preamble
        for index, marker in enumerate(recovered_pages):
            end = (recovered_pages[index + 1].start()
                   if index + 1 < len(recovered_pages) else len(text))
            page_text = text[marker.end():end]
            if page_text.strip():
                yield int(marker.group(1)), page_text
        return
    pages = text.split("\f")
    numbered = len(pages) > 1
    for index, page in enumerate(pages, 1):
        if page.strip():
            yield (index if numbered else None), page


def make_chunks(source: Source, target: int = 1800) -> list[Chunk]:
    """Split source text into bounded, page/section-addressable passages."""
    if "machine output, imperfect" in source.text[:500].lower():
        return []
    chunks: list[Chunk] = []
    ordinal = 0
    section = ""
    for page, page_text in _page_blocks(source.text):
        paragraphs = re.split(r"\n\s*\n+", page_text)
        pending: list[str] = []
        pending_size = 0

        def flush() -> None:
            nonlocal ordinal, pending, pending_size
            if not pending:
                return
            combined = "\n\n".join(pending).strip()
            for piece in _split_long(combined):
                locator_parts = [source.key]
                if page is not None:
                    locator_parts.append(f"page={page}")
                if section:
                    locator_parts.append(f"section={_slug(section)[:64]}")
                locator_parts.append(f"chunk={ordinal}")
                chunks.append(Chunk(
                    ordinal=ordinal,
                    locator="#".join(locator_parts),
                    page=page,
                    section=section,
                    concepts=_concepts(piece),
                    text=piece,
                    sha256=_sha256(piece.encode("utf-8")),
                ))
                ordinal += 1
            pending = []
            pending_size = 0

        markdown = source.key.lower().endswith(".md")
        for paragraph in paragraphs:
            paragraph = re.sub(r"[ \t]+", " ", paragraph).strip()
            if not paragraph:
                continue
            heading = re.match(r"^#{1,6}\s+(.+)$", paragraph) if markdown else None
            if heading:
                flush()
                section = heading.group(1).strip()
            if pending and pending_size + len(paragraph) + 2 > target:
                flush()
            pending.append(paragraph)
            pending_size += len(paragraph) + 2
        flush()
    return [chunk for chunk in chunks if _searchable(chunk.text)]


def _searchable(text: str) -> bool:
    """Reject decoder debris that would manufacture confident search hits."""
    if re.search(r"(?m)^\[(?:off|pattern)\s+[0-9.]+\]", text):
        return False
    if len(re.findall(r"[A-Za-z]{2,}", text)) < 5:
        return False
    broken = sum(1 for char in text
                 if char in ("\ufffd", "\u00b7") or
                 (ord(char) < 32 and char not in "\n\t"))
    return broken / max(1, len(text)) < 0.005


_MIGRATIONS = {
    1: """
        CREATE TABLE sources (
            id INTEGER PRIMARY KEY,
            source_key TEXT NOT NULL UNIQUE,
            work_id TEXT NOT NULL,
            title TEXT NOT NULL,
            source_kind TEXT NOT NULL,
            extraction TEXT NOT NULL,
            path TEXT NOT NULL,
            source_sha256 TEXT NOT NULL,
            byte_size INTEGER NOT NULL,
            indexer_version INTEGER NOT NULL
        );
        CREATE TABLE chunks (
            id INTEGER PRIMARY KEY,
            source_id INTEGER NOT NULL REFERENCES sources(id) ON DELETE CASCADE,
            ordinal INTEGER NOT NULL,
            locator TEXT NOT NULL UNIQUE,
            page INTEGER,
            section TEXT NOT NULL,
            concepts TEXT NOT NULL,
            text_sha256 TEXT NOT NULL,
            UNIQUE(source_id, ordinal)
        );
        CREATE VIRTUAL TABLE chunks_fts USING fts5(
            title, section, concepts, text,
            tokenize='unicode61 remove_diacritics 2'
        );
        CREATE INDEX chunks_source_id ON chunks(source_id);
    """,
}


def connect(db_path: str) -> sqlite3.Connection:
    parent = os.path.dirname(os.path.abspath(db_path))
    os.makedirs(parent, exist_ok=True)
    db = sqlite3.connect(db_path)
    if os.name == "posix":
        os.chmod(db_path, stat.S_IRUSR | stat.S_IWUSR)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys = ON")
    current = int(db.execute("PRAGMA user_version").fetchone()[0])
    if current > SCHEMA_VERSION:
        db.close()
        raise SourceIndexError(
            f"source index schema {current} is newer than this tool supports "
            f"({SCHEMA_VERSION})"
        )
    try:
        for version in range(current + 1, SCHEMA_VERSION + 1):
            db.executescript(
                "BEGIN IMMEDIATE;\n" + _MIGRATIONS[version] +
                f"\nPRAGMA user_version = {version};\nCOMMIT;"
            )
    except sqlite3.OperationalError as exc:
        db.close()
        if "fts5" in str(exc).lower():
            raise SourceIndexError(
                "this Python sqlite3 build does not provide FTS5"
            ) from exc
        raise
    return db


def _delete_source(db: sqlite3.Connection, source_id: int) -> None:
    rows = db.execute("SELECT id FROM chunks WHERE source_id = ?", (source_id,))
    for row in rows:
        db.execute("DELETE FROM chunks_fts WHERE rowid = ?", (row["id"],))
    db.execute("DELETE FROM sources WHERE id = ?", (source_id,))


def _insert_source(db: sqlite3.Connection, source: Source) -> None:
    cursor = db.execute(
        """INSERT INTO sources
           (source_key, work_id, title, source_kind, extraction, path,
            source_sha256, byte_size, indexer_version)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)""",
        (source.key, source.work_id, source.title, source.kind, source.extraction,
         source.path, source.sha256, source.byte_size, INDEXER_VERSION),
    )
    source_id = int(cursor.lastrowid)
    for chunk in make_chunks(source):
        row = db.execute(
            """INSERT INTO chunks
               (source_id, ordinal, locator, page, section, concepts, text_sha256)
               VALUES (?, ?, ?, ?, ?, ?, ?)""",
            (source_id, chunk.ordinal, chunk.locator, chunk.page, chunk.section,
             chunk.concepts, chunk.sha256),
        )
        db.execute(
            """INSERT INTO chunks_fts(rowid, title, section, concepts, text)
               VALUES (?, ?, ?, ?, ?)""",
            (row.lastrowid, source.title, chunk.section, chunk.concepts, chunk.text),
        )


def build_index(corpus: str = DEFAULT_CORPUS, db_path: str | None = None,
                rebuild: bool = False) -> BuildSummary:
    """Incrementally make the database exactly describe the current corpus."""
    db_path = db_path or default_db_path(corpus)
    if rebuild and os.path.exists(db_path):
        os.unlink(db_path)
    sources = discover_sources(corpus)
    db = connect(db_path)
    added = updated = unchanged = removed = 0
    try:
        db.execute("BEGIN IMMEDIATE")
        existing = {row["source_key"]: row for row in db.execute(
            "SELECT id, source_key, source_sha256, indexer_version, path FROM sources")}
        discovered = {source.key for source in sources}
        for key in sorted(set(existing) - discovered):
            _delete_source(db, int(existing[key]["id"]))
            removed += 1
        for source in sources:
            old = existing.get(source.key)
            if (old and old["source_sha256"] == source.sha256 and
                    int(old["indexer_version"]) == INDEXER_VERSION):
                if old["path"] != source.path:
                    db.execute("UPDATE sources SET path = ? WHERE id = ?",
                               (source.path, old["id"]))
                unchanged += 1
                continue
            if old:
                _delete_source(db, int(old["id"]))
                updated += 1
            else:
                added += 1
            _insert_source(db, source)
        db.commit()
        counts = db.execute(
            "SELECT (SELECT count(*) FROM sources), (SELECT count(*) FROM chunks)"
        ).fetchone()
        return BuildSummary(added, updated, unchanged, removed,
                            int(counts[0]), int(counts[1]))
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()


def _query_groups(query: str) -> list[tuple[str, ...]]:
    low = query.lower()
    concepts = []
    consumed: set[str] = set()
    for concept, terms in CONCEPT_TERMS.items():
        hits = [term for term in terms
                if re.search(r"(?<!\w)" + re.escape(term) + r"(?!\w)", low)]
        if hits:
            concepts.append((concept,) + terms)
            consumed.update(word for term in hits
                            for word in re.findall(r"[a-z0-9]+", term))
    words = [word for word in re.findall(r"[a-z0-9]+", low)
             if len(word) >= 2 and word not in _STOP_WORDS and word not in consumed]
    groups = concepts + [(word,) for word in words]
    # Preserve order while removing identical semantic groups and repeated words.
    return list(dict.fromkeys(groups))


def _fts_term(term: str) -> str:
    words = re.findall(r"[a-z0-9]+", term.lower())
    return '"' + " ".join(words).replace('"', '""') + '"'


def _fts_query(groups: list[tuple[str, ...]], require_all: bool) -> str:
    rendered = []
    for group in groups:
        terms = list(dict.fromkeys(term for term in group if re.search(r"[a-z0-9]", term)))
        rendered.append("(" + " OR ".join(_fts_term(term) for term in terms) + ")")
    return (" AND " if require_all else " OR ").join(rendered)


def search(db_path: str, query: str, limit: int = 8,
           source: str | None = None) -> list[dict]:
    """Return compact passages with stable locators and source hashes."""
    if not os.path.exists(db_path):
        raise SourceIndexError(
            f"no source index at {db_path}; run source_index.py build first"
        )
    groups = _query_groups(query)
    if not groups:
        return []
    limit = max(1, min(int(limit), MAX_RESULTS))
    db = connect(db_path)
    try:
        for require_all in (True, False):
            match = _fts_query(groups, require_all)
            sql = """
                SELECT s.source_key, s.work_id, s.title, s.source_kind,
                       s.extraction, s.source_sha256, c.locator, c.page,
                       c.section, c.concepts, c.text_sha256,
                       bm25(chunks_fts, 2.0, 1.5, 1.0, 1.0) AS rank,
                       snippet(chunks_fts, 3, '[', ']', ' ... ', 48) AS snippet
                  FROM chunks_fts
                  JOIN chunks c ON c.id = chunks_fts.rowid
                  JOIN sources s ON s.id = c.source_id
                 WHERE chunks_fts MATCH ?
            """
            params: list[object] = [match]
            if source:
                escaped = source.replace("\\", "\\\\").replace("%", "\\%") \
                    .replace("_", "\\_")
                sql += (" AND (s.work_id = ? OR s.title LIKE ? ESCAPE '\\' "
                        "OR s.source_key LIKE ? ESCAPE '\\')")
                params += [source, f"%{escaped}%", f"%{escaped}%"]
            sql += " ORDER BY rank, s.source_key, c.ordinal LIMIT ?"
            params.append(min(MAX_RESULTS * 4, limit * 4))
            candidates = [dict(row) for row in db.execute(sql, params)]
            rows = []
            seen: set[tuple[str, str]] = set()
            for row in candidates:
                fingerprint = " ".join(re.findall(
                    r"[a-z0-9]+", row["snippet"].lower()
                ))
                identity = (row["work_id"], fingerprint)
                if identity in seen:
                    continue
                seen.add(identity)
                rows.append(row)
                if len(rows) == limit:
                    break
            for row in rows:
                row["match_mode"] = "all-groups" if require_all else "any-group"
            if rows or not require_all:
                return rows
        return []
    finally:
        db.close()


def status(db_path: str) -> dict:
    if not os.path.exists(db_path):
        return {"present": False, "schema_version": 0, "sources": 0, "chunks": 0}
    db = connect(db_path)
    try:
        counts = db.execute(
            "SELECT (SELECT count(*) FROM sources), (SELECT count(*) FROM chunks)"
        ).fetchone()
        unsearchable = [dict(row) for row in db.execute(
            """SELECT s.source_key, s.work_id
                 FROM sources s LEFT JOIN chunks c ON c.source_id = s.id
                GROUP BY s.id HAVING count(c.id) = 0
                ORDER BY s.source_key"""
        )]
        return {"present": True, "schema_version": SCHEMA_VERSION,
                "sources": int(counts[0]), "chunks": int(counts[1]),
                "unsearchable_sources": unsearchable,
                "db": os.path.abspath(db_path)}
    finally:
        db.close()


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", default=DEFAULT_CORPUS)
    parser.add_argument("--db")
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser("build", help="incrementally index the local corpus")
    build.add_argument("--rebuild", action="store_true")
    query = sub.add_parser("query", help="find bounded passages and locators")
    query.add_argument("query")
    query.add_argument("--limit", type=int, default=8)
    query.add_argument("--source")
    query.add_argument("--json", action="store_true")
    sub.add_parser("status", help="report index schema and row counts")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    db_path = args.db or default_db_path(args.corpus)
    try:
        if args.command == "build":
            result = build_index(args.corpus, db_path, args.rebuild)
            print(json.dumps(dataclasses.asdict(result), sort_keys=True))
            return 0
        if args.command == "status":
            print(json.dumps(status(db_path), sort_keys=True))
            return 0
        rows = search(db_path, args.query, args.limit, args.source)
        if args.json:
            print(json.dumps(rows, indent=2, ensure_ascii=False, sort_keys=True))
        else:
            for row in rows:
                page = f" page {row['page']}" if row["page"] is not None else ""
                section = f" — {row['section']}" if row["section"] else ""
                fallback = (" [any-group fallback]"
                            if row["match_mode"] == "any-group" else "")
                print(f"{row['title']}{page}{section}{fallback}")
                print(f"  {row['locator']}")
                print(f"  {row['snippet']}")
        return 0
    except (SourceIndexError, sqlite3.Error, OSError) as exc:
        print(f"source index: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
