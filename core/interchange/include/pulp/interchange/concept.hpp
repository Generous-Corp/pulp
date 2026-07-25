#pragma once

// The interchange concept vocabulary: the format-neutral names every reader,
// writer, refusal, and loss entry is expressed in. Concepts are capability
// atoms, deliberately finer than "tracks, clips", because a refusal message is
// only as precise as the vocabulary it can name the offending construct in.
//
// The enum, its string ids, and the per-concept summaries are generated from
// core/interchange/capabilities/concepts.json so C++, the docs page, and the
// tests share one source. Edit the JSON, not the generated header.

#include <pulp/interchange/generated/concepts.hpp>
