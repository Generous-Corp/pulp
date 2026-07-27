#pragma once

/// @file api_groups.hpp
/// Navigation groups for the installed Timeline API.

/**
 * @defgroup timeline Timeline
 * Immutable musical documents, transactional editing, persistence, compilation,
 * and interchange.
 */

/**
 * @defgroup timeline_model Model and authoring
 * @ingroup timeline
 * Immutable values and authored musical intent.
 */

/**
 * @defgroup timeline_editing Editing and sessions
 * @ingroup timeline
 * Typed commands, optimistic transactions, sessions, and undo history.
 */

/**
 * @defgroup timeline_persistence Persistence and schema
 * @ingroup timeline
 * Durable journals, canonical serialization, schemas, and migrations.
 */

/**
 * @defgroup timeline_compile Compilation and extensions
 * @ingroup timeline
 * Compile-time context contracts and extension preparation.
 */

/**
 * @defgroup timeline_interchange Interchange
 * @ingroup timeline
 * Explicit import and export boundaries for external formats.
 */
