#pragma once

// Folding a fresh port-map scan into whatever was already mapped.
//
// Split out of CARTOG.cpp so it can be tested without Rack. Everything here is
// pure text over std::string: no scene, no widgets, no plugin SDK. The scanner
// needs Rack running and a rack full of modules to exercise; this does not, and
// this is the part with the edge cases.
//
// Deliberately textual rather than parsed. Rack ships no JSON writer a module
// can reach, so the scanner emits its map by building a string -- and adding a
// parser purely to merge would mean a second representation to keep in step
// with the first, which is how the two come to disagree.

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace forge_portmap {

/// The per-module objects in a scan's text.
///
/// Each begins at a line that is exactly four spaces and a brace, which is the
/// only place the scanner writes one -- nested objects (a port, a param) are
/// indented deeper, so they are never mistaken for a module.
inline std::vector<std::string> module_blocks(const std::string& text) {
    std::vector<std::string> out;
    const std::string open = "\n    {\n";
    const std::string close = "\n    }";
    std::size_t at = 0;
    while ((at = text.find(open, at)) != std::string::npos) {
        const std::size_t start = at + 1;               // past the newline
        const std::size_t end = text.find(close, start);
        if (end == std::string::npos) break;            // truncated: keep none
        out.push_back(text.substr(start, end + close.size() - start));
        at = end + close.size();
    }
    return out;
}

/// "plugin/model" for one block, or empty when either field is absent.
inline std::string key_of(const std::string& block) {
    auto field = [&](const char* name) {
        const std::string k = std::string("\"") + name + "\": \"";
        const auto at = block.find(k);
        if (at == std::string::npos) return std::string{};
        const auto from = at + k.size();
        const auto to = block.find('"', from);
        if (to == std::string::npos) return std::string{};
        return block.substr(from, to - from);
    };
    const auto p = field("plugin"), m = field("model");
    if (p.empty() || m.empty()) return {};
    return p + "/" + m;
}

/// Every "plugin/model" a scan's text contains.
inline std::set<std::string> module_keys(const std::string& text) {
    std::set<std::string> out;
    for (const auto& block : module_blocks(text)) {
        const auto k = key_of(block);
        if (!k.empty()) out.insert(k);
    }
    return out;
}

/// Fold `fresh` into `old_text`, newest measurement winning.
///
/// A scan records only what is on screen, and no screen holds a whole library,
/// so mapping one means scanning in batches. Writing the file outright made
/// every batch erase the one before it -- fine for a single rack, useless for
/// the job this is for.
///
/// A module in BOTH is taken from the fresh scan: it was just measured, and a
/// fresh measurement of a possibly-updated plugin beats a stale one. Everything
/// else carries through untouched, so a batch adds without costing.
///
/// Anything unexpected returns `fresh` unchanged. A merge that cannot be done
/// safely must not produce a file that is neither the old map nor the new one.
inline std::string merge(const std::string& old_text, const std::string& fresh) {
    if (old_text.empty()) return fresh;

    const auto keys = module_keys(fresh);
    std::string kept;
    for (const auto& block : module_blocks(old_text)) {
        const auto key = key_of(block);
        if (key.empty() || keys.count(key)) continue;   // re-measured just now
        if (!kept.empty()) kept += ",\n";
        kept += block;
    }
    if (kept.empty()) return fresh;

    const std::string head = "{\n  \"modules\": [\n";
    if (fresh.rfind(head, 0) != 0) return fresh;        // shape changed: do no harm
    const std::string body = fresh.substr(head.size());
    if (body.find("\n  ]\n}") == std::string::npos) return fresh;

    // A scan with nothing on screen still has to keep the old map rather than
    // blank it, so the separator depends on whether anything follows.
    const bool fresh_has_modules = !module_blocks(fresh).empty();
    return head + kept + (fresh_has_modules ? ",\n" : "\n") + body;
}

}  // namespace forge_portmap
