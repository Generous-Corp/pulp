/// @file anchor_strategy.cpp
/// Implementation of stable_anchor_id strategies. Mirrors
/// packages/pulp-import-ir/src/anchors.ts so the C++ and TS pipelines
/// produce compatible anchors and the tweaks layer matches edits across
/// either path.
///
/// Hash algorithm: FNV-1a 32-bit, base-36 encoded. Matches the TS hash
/// exactly (FNV offset basis 0x811c9dc5, prime 0x01000193). The TS file
/// notes the algorithm is lifted from MIT-licensed
/// mitosis/packages/core/src/symbols/symbol-processor.ts.

#include <pulp/view/anchor_strategy.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>
#include <unordered_map>

namespace pulp::view {

namespace {

std::string_view strategy_name(AnchorStrategy strategy) {
    switch (strategy) {
        case AnchorStrategy::content_hash: return "content-hash";
        case AnchorStrategy::path:         return "path";
        case AnchorStrategy::adapter:      return "adapter";
    }
    return "content-hash";
}

// ── FNV-1a 32-bit hash, returned as base-36 string (matches TS) ──────────
std::string fnv1a_base36(std::string_view input) {
    std::uint32_t hash = 0x811c9dc5u;
    for (char c : input) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 0x01000193u;  // 32-bit overflow wraps, matching Math.imul()
    }
    if (hash == 0) return "0";
    std::string out;
    while (hash > 0) {
        std::uint32_t d = hash % 36u;
        out.push_back(d < 10 ? static_cast<char>('0' + d)
                             : static_cast<char>('a' + d - 10));
        hash /= 36u;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// ── Normalize text the same way the TS path does: collapse runs of
//    whitespace, trim, lowercase. So minor whitespace edits in the
//    source don't rotate the hash.
std::string normalize_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool in_ws = true;  // skip leading whitespace
    for (char c : text) {
        bool is_ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (is_ws) {
            if (!in_ws) { out.push_back(' '); in_ws = true; }
        } else {
            // ASCII lowercase only — matches JS toLowerCase() for the
            // ASCII subset, which is what every existing adapter emits.
            out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
            in_ws = false;
        }
    }
    // Trim trailing whitespace introduced by the loop.
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// ── "Role" is held in IRNode::attributes["role"] (matches the
//    convention parse_ir_node uses for ARIA-role-bearing tags).
std::string node_role(const IRNode& node) {
    auto it = node.attributes.find("role");
    if (it == node.attributes.end()) return {};
    return it->second;
}

// ── Stable stringify for the content-hash input. Sorts keys
//    lexicographically so we never get key-order non-determinism.
//    Matches the TS stableStringify pattern.
std::string stable_stringify_content_hash_input(std::string_view tag,
                                                std::string_view role,
                                                std::string_view text,
                                                std::size_t depth,
                                                std::size_t sig_index) {
    // Use std::map to get deterministic key ordering.
    std::map<std::string, std::string> fields;
    auto quote = [](std::string_view s) {
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('"');
        for (char c : s) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        out.push_back('"');
        return out;
    };
    fields["depth"] = std::to_string(depth);
    fields["role"] = quote(role);
    fields["sigIndex"] = std::to_string(sig_index);
    fields["tag"] = quote(tag);
    fields["text"] = quote(text);

    std::ostringstream ss;
    ss << '{';
    bool first = true;
    for (auto& [k, v] : fields) {
        if (!first) ss << ',';
        first = false;
        ss << '"' << k << "\":" << v;
    }
    ss << '}';
    return ss.str();
}

// Signature key for sibling-disambiguation in content-hash mode. Must
// mirror the TS contentHashSignatureKey() exactly — JSON-encoded array
// of [tag, role, text] so adjacent fields can't collapse to the same
// key (e.g. tag="ab",role="" vs. tag="a",role="b").
std::string content_hash_signature_key(const IRNode& node) {
    std::string tag = node.type;
    std::string role = node_role(node);
    std::string text = normalize_text(node.text_content);
    auto escape = [](std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    };
    std::ostringstream ss;
    ss << "[\"" << escape(tag) << "\",\"" << escape(role) << "\",\""
       << escape(text) << "\"]";
    return ss.str();
}

void walk(IRNode& node,
          std::string_view parent_anchor,
          std::size_t parent_child_index,
          std::size_t depth,
          AnchorStrategy strategy,
          std::string_view adapter_name,
          IRNode* parent,
          std::size_t sig_index) {
    // For path strategy: count earlier siblings with the same tag.
    std::size_t sibling_tag_index = 0;
    if (parent != nullptr && strategy == AnchorStrategy::path) {
        for (std::size_t i = 0; i < parent_child_index; ++i) {
            if (parent->children[i].type == node.type) ++sibling_tag_index;
        }
    }

    // Preserve any pre-existing anchor (e.g. from an authored override).
    if (!node.stable_anchor_id || node.stable_anchor_id->empty()) {
        node.stable_anchor_id = compute_anchor_id(
            node, parent_anchor, sibling_tag_index, depth, sig_index,
            strategy, adapter_name);
    }
    if (!node.anchor_strategy || node.anchor_strategy->empty()) {
        node.anchor_strategy = std::string(strategy_name(strategy));
    }

    // Child-anchors are path-scoped under this node for the path strategy;
    // for content-hash/adapter, each child computes independently.
    std::string child_parent_anchor =
        (strategy == AnchorStrategy::path) ? *node.stable_anchor_id : std::string{};

    // For content-hash, count duplicate-signature siblings so each
    // duplicate gets a distinct sig_index discriminator.
    std::unordered_map<std::string, std::size_t> sig_counts;
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        IRNode& c = node.children[i];
        std::size_t child_sig_index = 0;
        if (strategy == AnchorStrategy::content_hash) {
            auto key = content_hash_signature_key(c);
            auto& n = sig_counts[key];
            child_sig_index = n;
            ++n;
        }
        walk(c, child_parent_anchor, i, depth + 1, strategy, adapter_name,
             &node, child_sig_index);
    }
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────────

std::string compute_anchor_id(const IRNode& node,
                              std::string_view parent_anchor,
                              std::size_t sibling_tag_index_for_path,
                              std::size_t depth,
                              std::size_t sig_index_for_content_hash,
                              AnchorStrategy strategy,
                              std::string_view adapter_name) {
    if (strategy == AnchorStrategy::adapter) {
        // Caller must populate source_node_id before invoking the
        // adapter strategy. If they didn't, fall back to content-hash so
        // we always produce SOME anchor — better than a crash inside the
        // import pipeline.
        if (node.source_node_id && !node.source_node_id->empty() &&
            !adapter_name.empty()) {
            std::string out;
            out.reserve(adapter_name.size() + 1 + node.source_node_id->size());
            out.append(adapter_name);
            out.push_back(':');
            out.append(*node.source_node_id);
            return out;
        }
        // Fall through to content-hash as a safety net. Caller-side tests
        // assert that adapter sources populate source_node_id properly,
        // so this branch is for defense in depth, not normal operation.
        // (Matches TS behavior of throwing — we soft-fail instead because
        // a missing anchor breaks the inspector silently, while a
        // recovered anchor at least lets the pipeline finish.)
    }

    if (strategy == AnchorStrategy::path) {
        std::string seg = node.type;
        seg.push_back('[');
        seg += std::to_string(sibling_tag_index_for_path);
        seg.push_back(']');
        if (parent_anchor.empty()) return seg;
        std::string out;
        out.reserve(parent_anchor.size() + 1 + seg.size());
        out.append(parent_anchor);
        out.push_back('/');
        out.append(seg);
        return out;
    }

    // content_hash (default + adapter fallback)
    std::string tag = node.type;
    std::string role = node_role(node);
    std::string text = normalize_text(node.text_content);
    std::string input = stable_stringify_content_hash_input(
        tag, role, text, depth, sig_index_for_content_hash);
    return fnv1a_base36(input);
}

void assign_anchors(IRNode& root,
                    AnchorStrategy strategy,
                    std::string_view adapter_name) {
    walk(root, /*parent_anchor=*/{}, /*parent_child_index=*/0, /*depth=*/0,
         strategy, adapter_name, /*parent=*/nullptr, /*sig_index=*/0);
}

AnchorStrategy default_anchor_strategy(DesignSource source) {
    // Mirror DEFAULT_ANCHOR_STRATEGY in @pulp/import-ir/src/anchors.ts.
    switch (source) {
        case DesignSource::figma:    return AnchorStrategy::adapter;
        case DesignSource::pencil:   return AnchorStrategy::adapter;
        case DesignSource::v0:       return AnchorStrategy::content_hash;
        case DesignSource::stitch:   return AnchorStrategy::content_hash;
        case DesignSource::claude:   return AnchorStrategy::content_hash;
        case DesignSource::jsx:      return AnchorStrategy::content_hash;
        case DesignSource::designmd: return AnchorStrategy::content_hash;
    }
    return AnchorStrategy::content_hash;
}

}  // namespace pulp::view
