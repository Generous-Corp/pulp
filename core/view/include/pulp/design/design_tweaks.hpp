#pragma once

// pulp::design — the self-describing parameter block an imported/generated
// design carries so that cheap tweaks survive a reload and live in the diff.
//
// An imported artifact (a `ui.js`, a `.dc.html`) embeds one marker-delimited
// block:
//
//     /*EDITMODE-BEGIN*/{"accent":"#33aaff","radius":8}/*EDITMODE-END*/
//
// The block is the artifact's own parameter store. When a human nudges a knob
// in the inspector the host rewrites *only the JSON between the markers*,
// byte-for-byte preserving everything outside it. Because the block is anchored
// content (fixed markers, not a line/offset), it composes with the re-import
// safety loop: a re-import re-emits the surrounding artifact and the persisted
// block is re-applied by key, not by position.
//
// This translation unit is the pure core of that mechanism: locate the block,
// read its parameters, and produce the rewritten artifact text for a changed
// set of parameters. The inspector panel and the send-to-agent affordance are
// UI layers built on top of these primitives and live elsewhere.
//
// Everything here is deterministic and free of I/O so it is unit-testable
// without a window or a render context.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::design {

/// The exact opening and closing markers that anchor the block. Fixed strings —
/// a change here is a format break, not a tuning knob.
inline constexpr std::string_view kEditBlockBegin = "/*EDITMODE-BEGIN*/";
inline constexpr std::string_view kEditBlockEnd = "/*EDITMODE-END*/";

/// One parameter in the block: a key and its JSON-encoded value. The value is
/// kept as its JSON text — a quoted color string with its quotes, or 8, true,
/// 0.5 — so a rewrite never has to guess a number's formatting or re-quote a
/// string.
struct TweakParam {
    std::string key;
    std::string json_value;  ///< the value's JSON text, exactly as it will serialize
};

/// Where the block sits in the host text, as byte offsets. `payload_*` bound the
/// JSON object *between* the markers (excluding the markers themselves); `block_*`
/// bound the whole marker-delimited span (including both markers).
struct EditBlockSpan {
    size_t block_begin = 0;    ///< offset of the first byte of kEditBlockBegin
    size_t block_end = 0;      ///< offset one past the last byte of kEditBlockEnd
    size_t payload_begin = 0;  ///< offset of the first byte of the JSON payload
    size_t payload_end = 0;    ///< offset one past the last byte of the JSON payload
};

/// Locate the first edit block in `text`. The payload between the markers must
/// be exactly one balanced JSON object followed only by whitespace and the
/// closing marker. Returns nullopt when there is no opening marker, no valid
/// object at the payload, trailing bytes after the object before the marker, or
/// no closing marker. The object scan is string-aware, so a value that itself
/// contains the literal end-marker text does not truncate the block.
std::optional<EditBlockSpan> find_edit_block(std::string_view text);

/// Parse the parameters out of `text`'s edit block. Returns nullopt when there
/// is no well-formed block, its payload is not a JSON object, or the object has
/// a duplicate key (a JSON object may not repeat a key — the parser rejects it).
/// Object member order is preserved: it is the order the rewriter re-emits.
std::optional<std::vector<TweakParam>> read_edit_block(std::string_view text);

/// Serialize parameters to the canonical single-line block payload (no outer
/// markers): `{"k1":v1,"k2":v2}`. Keys are emitted in the given order. Each value
/// is normalized to exactly one JSON value, so a stored value cannot smuggle a
/// marker or extra structure into the block. Empty params serialize `{}`. Returns
/// nullopt when a key is not valid UTF-8 or a value is not exactly one JSON value.
std::optional<std::string> edit_block_payload(const std::vector<TweakParam>& params);

/// Result of a rewrite: the new full artifact text plus whether anything outside
/// the block changed (it must not — this is a self-check the caller can assert).
struct RewriteResult {
    std::string text;             ///< the artifact with the block's payload replaced
    bool outside_bytes_intact = false;  ///< true iff every byte outside the block is unchanged
};

/// Rewrite `text`'s edit block so its payload encodes `params`, preserving every
/// byte outside the marker span exactly. Returns nullopt when `text` has no
/// well-formed block (there is nothing to anchor a rewrite to). The markers
/// themselves are preserved; only the bytes between them are replaced.
std::optional<RewriteResult> rewrite_edit_block(std::string_view text,
                                                const std::vector<TweakParam>& params);

/// Apply a single key=value change to `text`'s block: update the key in place if
/// present, else append it (preserving the order of existing keys). `json_value`
/// is the value's JSON text (quote strings yourself). Returns nullopt when there
/// is no well-formed block. A convenience over read + mutate + rewrite.
std::optional<RewriteResult> set_edit_param(std::string_view text, std::string_view key,
                                            std::string_view json_value);

}  // namespace pulp::design
