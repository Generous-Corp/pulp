// Cross-platform text-accessibility scaffold (font v2 Slice 2.6).
//
// Validates the default no-op backend: a process-local registry that
// stores TextAccessibilityNode entries keyed by id. The platform
// overlays (NSAccessibility, UIA, AccessKit) will replace
// accessibility_backend_name() and the register/unregister symbols in
// a follow-up slice; this test pins down the cross-platform surface.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/text_accessibility.hpp>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::view;

namespace {

void clear_registry() {
    for (const auto& node : snapshot_accessibility_nodes()) {
        unregister_text_accessibility_node(node.id);
    }
    REQUIRE(snapshot_accessibility_nodes().empty());
}

const TextAccessibilityNode* find_by_id(
    const std::vector<TextAccessibilityNode>& nodes, std::string_view id) {
    for (const auto& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("TextAccessibilityNode: register label round-trips through snapshot",
          "[view][text-a11y][issue-2255]") {
    clear_registry();

    TextAccessibilityNode node;
    node.id = "label-1";
    node.text = "Hello";
    node.role = TextAccessibilityRole::Label;
    node.cluster_boundaries_utf8 = {0, 1, 2, 3, 4, 5};
    node.cluster_boundaries_utf16 = {0, 1, 2, 3, 4, 5};

    register_text_accessibility_node(node);

    auto snap = snapshot_accessibility_nodes();
    REQUIRE(snap.size() == 1);
    REQUIRE(snap[0].id == "label-1");
    REQUIRE(snap[0].text == "Hello");
    REQUIRE(snap[0].role == TextAccessibilityRole::Label);
    REQUIRE(snap[0].cluster_boundaries_utf8.size() == 6);

    clear_registry();
}

TEST_CASE("TextAccessibilityNode: two distinct ids coexist in registry",
          "[view][text-a11y][issue-2255]") {
    clear_registry();

    TextAccessibilityNode a;
    a.id = "alpha";
    a.text = "A";
    a.role = TextAccessibilityRole::Label;

    TextAccessibilityNode b;
    b.id = "beta";
    b.text = "B";
    b.role = TextAccessibilityRole::Button;

    register_text_accessibility_node(a);
    register_text_accessibility_node(b);

    auto snap = snapshot_accessibility_nodes();
    REQUIRE(snap.size() == 2);
    const auto* found_a = find_by_id(snap, "alpha");
    const auto* found_b = find_by_id(snap, "beta");
    REQUIRE(found_a != nullptr);
    REQUIRE(found_b != nullptr);
    REQUIRE(found_a->text == "A");
    REQUIRE(found_b->text == "B");
    REQUIRE(found_b->role == TextAccessibilityRole::Button);

    clear_registry();
}

TEST_CASE("TextAccessibilityNode: same-id register replaces previous entry",
          "[view][text-a11y][issue-2255]") {
    clear_registry();

    TextAccessibilityNode first;
    first.id = "dup";
    first.text = "old";
    first.role = TextAccessibilityRole::Label;
    register_text_accessibility_node(first);

    TextAccessibilityNode second;
    second.id = "dup";
    second.text = "new";
    second.role = TextAccessibilityRole::Heading;
    register_text_accessibility_node(second);

    auto snap = snapshot_accessibility_nodes();
    REQUIRE(snap.size() == 1);
    REQUIRE(snap[0].id == "dup");
    REQUIRE(snap[0].text == "new");
    REQUIRE(snap[0].role == TextAccessibilityRole::Heading);

    clear_registry();
}

TEST_CASE("TextAccessibilityNode: unregister drops entry by id",
          "[view][text-a11y][issue-2255]") {
    clear_registry();

    TextAccessibilityNode node;
    node.id = "to-drop";
    node.text = "bye";
    register_text_accessibility_node(node);
    REQUIRE(snapshot_accessibility_nodes().size() == 1);

    unregister_text_accessibility_node("to-drop");
    REQUIRE(snapshot_accessibility_nodes().empty());

    // Unregistering a missing id is a silent no-op.
    unregister_text_accessibility_node("never-registered");
    REQUIRE(snapshot_accessibility_nodes().empty());
}

TEST_CASE("TextAccessibilityNode: backend probe is stable across calls",
          "[view][text-a11y][issue-2255]") {
    const auto first = accessibility_backend_name();
    const auto second = accessibility_backend_name();
    REQUIRE(first == second);
    // Per-platform overlays replace the default "none" symbol at link
    // time. macOS ships the NSAccessibility overlay ("macos-ax", font
    // v2 Slice 2.6); Windows UIA + Linux AccessKit are deferred to
    // follow-up slices and still ride the default. The probe value
    // therefore depends on the platform link line, but it must be one
    // of the recognized backend identifiers.
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    REQUIRE(first == "macos-ax");
#else
    REQUIRE(first == "none");
#endif
}

TEST_CASE("TextAccessibilityNode: cluster boundary vectors round-trip",
          "[view][text-a11y][issue-2255]") {
    clear_registry();

    TextAccessibilityNode node;
    node.id = "clusters";
    node.text = "Hi  there";  // contrived: boundaries don't need to match text
    node.cluster_boundaries_utf8 = {0, 2, 5, 7};
    node.cluster_boundaries_utf16 = {0, 1, 3, 4};
    node.selection_start_utf8 = 2;
    node.selection_end_utf8 = 5;
    register_text_accessibility_node(node);

    auto snap = snapshot_accessibility_nodes();
    REQUIRE(snap.size() == 1);
    REQUIRE(snap[0].cluster_boundaries_utf8 == std::vector<std::size_t>{0, 2, 5, 7});
    REQUIRE(snap[0].cluster_boundaries_utf16 == std::vector<std::size_t>{0, 1, 3, 4});
    REQUIRE(snap[0].selection_start_utf8 == 2);
    REQUIRE(snap[0].selection_end_utf8 == 5);

    clear_registry();
}

TEST_CASE("TextAccessibilityNode: concurrent register from N threads is race-free",
          "[view][text-a11y][issue-2255]") {
    clear_registry();

    constexpr int kThreads = 10;
    constexpr int kPerThread = 25;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    std::atomic<int> registered{0};

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &registered] {
            for (int i = 0; i < kPerThread; ++i) {
                TextAccessibilityNode node;
                node.id = "t" + std::to_string(t) + "-i" + std::to_string(i);
                node.text = node.id;
                node.role = TextAccessibilityRole::Other;
                register_text_accessibility_node(node);
                registered.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& w : workers) w.join();

    REQUIRE(registered.load() == kThreads * kPerThread);
    auto snap = snapshot_accessibility_nodes();
    REQUIRE(static_cast<int>(snap.size()) == kThreads * kPerThread);

    // Every node id is distinct; the registry must hold them all.
    std::vector<std::string> ids;
    ids.reserve(snap.size());
    for (const auto& n : snap) ids.push_back(n.id);
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    clear_registry();
}
