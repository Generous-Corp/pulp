#pragma once

// Identity System for Pulp
// Provides UUID generation and typed identity primitives for:
// - Session tracking (SessionId)
// - AI/tool execution tracking (RunId)
// - Domain object identity (ObjectId)
// - Cross-subsystem event correlation (CorrelationId)
//
// Uses UUIDv4 (random) for all identity types.
// Lightweight: each ID is a 128-bit value with type safety via distinct types.

#include <cstdint>
#include <string>
#include <array>
#include <cstring>
#include <functional>

namespace pulp::runtime {

// ── Raw UUID ────────────────────────────────────────────────────────────

// 128-bit UUID stored as two 64-bit integers
struct Uuid {
    uint64_t hi = 0;
    uint64_t lo = 0;

    bool is_nil() const { return hi == 0 && lo == 0; }

    bool operator==(const Uuid& other) const {
        return hi == other.hi && lo == other.lo;
    }
    bool operator!=(const Uuid& other) const { return !(*this == other); }
    bool operator<(const Uuid& other) const {
        return hi < other.hi || (hi == other.hi && lo < other.lo);
    }

    // Generate a UUIDv4 (random)
    static Uuid generate();

    // Parse from string "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
    static Uuid from_string(std::string_view str);

    // Format as standard UUID string
    std::string to_string() const;

    // Format as compact hex (no dashes, 32 chars)
    std::string to_hex() const;
};

// ── Typed Identity Wrappers ─────────────────────────────────────────────
// Each identity type is a distinct type wrapping a Uuid.
// This prevents accidental conflation of SessionId with ObjectId, etc.

// Identifies one user session, workspace open, or AI conversation context.
// Created on app launch or project open. Does NOT survive save/load —
// a new session starts when the project is reopened.
struct SessionId {
    Uuid value;

    static SessionId generate() { return {Uuid::generate()}; }
    static SessionId nil() { return {}; }
    bool is_nil() const { return value.is_nil(); }
    std::string to_string() const { return value.to_string(); }

    bool operator==(const SessionId& o) const { return value == o.value; }
    bool operator!=(const SessionId& o) const { return value != o.value; }
};

// Identifies one discrete AI/model/tool execution within a session.
// Created per model call, tool invocation, or undo group.
// Transient — does not survive save/load.
struct RunId {
    Uuid value;

    static RunId generate() { return {Uuid::generate()}; }
    static RunId nil() { return {}; }
    bool is_nil() const { return value.is_nil(); }
    std::string to_string() const { return value.to_string(); }

    bool operator==(const RunId& o) const { return value == o.value; }
    bool operator!=(const RunId& o) const { return value != o.value; }
};

// Stable identifier for a persisted domain object: document, preset, plugin
// instance, audio clip, generated output, etc.
// Created when the object is first created. Survives save/load.
// On duplication, the clone gets a NEW ObjectId; source_id preserves lineage.
struct ObjectId {
    Uuid value;

    static ObjectId generate() { return {Uuid::generate()}; }
    static ObjectId nil() { return {}; }
    static ObjectId from_string(std::string_view s) { return {Uuid::from_string(s)}; }
    bool is_nil() const { return value.is_nil(); }
    std::string to_string() const { return value.to_string(); }

    bool operator==(const ObjectId& o) const { return value == o.value; }
    bool operator!=(const ObjectId& o) const { return value != o.value; }
    bool operator<(const ObjectId& o) const { return value < o.value; }
};

// Used for log/trace/callback correlation across subsystems.
// Transient — attached to log entries, async operations, streaming chunks.
struct CorrelationId {
    Uuid value;

    static CorrelationId generate() { return {Uuid::generate()}; }
    static CorrelationId nil() { return {}; }
    bool is_nil() const { return value.is_nil(); }
    std::string to_string() const { return value.to_string(); }

    bool operator==(const CorrelationId& o) const { return value == o.value; }
    bool operator!=(const CorrelationId& o) const { return value != o.value; }
};

// ── Event Envelope ──────────────────────────────────────────────────────
// Every mutation or event carries identity metadata for attribution,
// correlation, and undo/redo support.

struct EventEnvelope {
    double timestamp = 0.0;          // Unix epoch seconds
    SessionId session_id;
    ObjectId object_id;              // The object being mutated
    RunId run_id;                    // Nil if not an AI operation
    CorrelationId correlation_id;    // For log/trace grouping
    std::string actor;               // "user", "ai", "host", subsystem name
    std::string action;              // "create", "modify", "delete", etc.
};

} // namespace pulp::runtime

// ── Hash support for use in unordered containers ────────────────────────

namespace std {
template<> struct hash<pulp::runtime::Uuid> {
    size_t operator()(const pulp::runtime::Uuid& id) const noexcept {
        return hash<uint64_t>{}(id.hi) ^ (hash<uint64_t>{}(id.lo) << 1);
    }
};
template<> struct hash<pulp::runtime::ObjectId> {
    size_t operator()(const pulp::runtime::ObjectId& id) const noexcept {
        return hash<pulp::runtime::Uuid>{}(id.value);
    }
};
template<> struct hash<pulp::runtime::SessionId> {
    size_t operator()(const pulp::runtime::SessionId& id) const noexcept {
        return hash<pulp::runtime::Uuid>{}(id.value);
    }
};
template<> struct hash<pulp::runtime::RunId> {
    size_t operator()(const pulp::runtime::RunId& id) const noexcept {
        return hash<pulp::runtime::Uuid>{}(id.value);
    }
};
template<> struct hash<pulp::runtime::CorrelationId> {
    size_t operator()(const pulp::runtime::CorrelationId& id) const noexcept {
        return hash<pulp::runtime::Uuid>{}(id.value);
    }
};
} // namespace std
