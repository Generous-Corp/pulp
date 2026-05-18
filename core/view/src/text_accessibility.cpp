// Cross-platform text accessibility scaffold (font v2 Slice 2.6).
//
// Implements the default "none" backend: a process-local mutex-guarded
// table of TextAccessibilityNode entries keyed by id. Per-platform
// backends (NSAccessibility, UIA, AccessKit) overlay this in future
// slices; until they land, this gives the text-paint sites a stable API
// surface to call against and gives tests a snapshot they can introspect.

#include <pulp/view/text_accessibility.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::view {

namespace {

struct Registry {
    std::mutex mu;
    std::unordered_map<std::string, TextAccessibilityNode> nodes;
};

Registry& registry() {
    // Function-local static: thread-safe initialization, no global
    // constructor ordering hazard. Lives for the lifetime of the
    // process, which is what we want for the default backend.
    static Registry r;
    return r;
}

}  // namespace

std::string_view accessibility_backend_name() noexcept {
    // The default backend does not talk to any platform a11y API. Per-
    // platform overlays replace this symbol at link time (or via a
    // future plug-in registration point) with "macos-ax",
    // "windows-uia", or "linux-accesskit".
    return "none";
}

void register_text_accessibility_node(const TextAccessibilityNode& node) {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    // Idempotent on id: operator[] + assignment so that a second
    // register with the same id replaces the first.
    r.nodes[node.id] = node;
}

void unregister_text_accessibility_node(std::string_view id) {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    // unordered_map::erase only takes a key by value for the
    // heterogeneous-lookup-less default, so materialize once.
    r.nodes.erase(std::string(id));
}

std::vector<TextAccessibilityNode> snapshot_accessibility_nodes() {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    std::vector<TextAccessibilityNode> out;
    out.reserve(r.nodes.size());
    for (const auto& [_, node] : r.nodes) {
        out.push_back(node);
    }
    return out;
}

}  // namespace pulp::view
