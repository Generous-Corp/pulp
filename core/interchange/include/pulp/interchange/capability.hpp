#pragma once

// Per-format capability tables: what each interchange format may read, and what
// writing it costs the document.
//
// The tables are generated from core/interchange/capabilities/<format>.json into
// constexpr arrays, so a lookup is an index and there is nothing to parse at
// runtime. The generator materializes a closed world -- every concept has a row
// for every format -- which is what makes the default safe: a concept a format
// never declared reads as ImportLevel::None, so its reader refuses, and as
// ExportLevel::Drop, so its writer must obtain consent. Support is declared,
// never inherited.
//
// Adding a format is adding one JSON file plus its reader/writer. Nothing in
// this header changes.

#include <pulp/interchange/generated/capability_tables.hpp>
