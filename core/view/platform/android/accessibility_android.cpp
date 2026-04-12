// Android TalkBack accessibility bridge — issue #87.
//
// Implements JNI native methods declared in PulpAccessibilityDelegate.kt.
// Walks the C++ View tree, collects Views with non-none AccessRole, and
// serves their properties (role, label, value, range) to the Kotlin side
// which maps them to Android AccessibilityNodeInfo objects.
//
// Architecture mirrors the iOS VoiceOver bridge in
// core/view/platform/ios/accessibility_ios.mm: flatten the View tree into
// a vector, query by index. The Kotlin delegate iterates the flat list
// and builds AccessibilityNodeInfo entries for each.

#ifdef __ANDROID__

#include <jni.h>
#include <string>
#include <vector>
#include <pulp/view/view.hpp>
#include <pulp/view/accessibility.hpp>

using pulp::view::View;
using pulp::view::Point;

// ── Flat node cache ─────────────────────────────────────────────────────
// Rebuilt on each onInitializeAccessibilityNodeInfo call (Kotlin calls
// nativeGetAccessibilityNodeCount first, then queries individual nodes).
// The cache is thread-local because TalkBack queries come on the main
// thread and the render thread must not interfere.

namespace {

struct AccessNode {
    View* view = nullptr;
    int role = 0;  // maps to View::AccessRole
};

thread_local std::vector<AccessNode> g_access_nodes;
thread_local View* g_root_view = nullptr;

void collect_accessible_views(View& root, std::vector<AccessNode>& out) {
    if (root.access_role() != View::AccessRole::none) {
        out.push_back({&root, static_cast<int>(root.access_role())});
    }
    for (size_t i = 0; i < root.child_count(); ++i) {
        if (auto* child = root.child_at(i)) {
            collect_accessible_views(*child, out);
        }
    }
}

void rebuild_cache() {
    g_access_nodes.clear();
    if (g_root_view) {
        collect_accessible_views(*g_root_view, g_access_nodes);
    }
}

} // namespace

// Called by the Android host when the root view is set or changed.
extern "C" void pulp_accessibility_set_root(View* root) {
    g_root_view = root;
}

// ── JNI exports ─────────────────────────────────────────────────────────

extern "C" {

JNIEXPORT jint JNICALL
Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetAccessibilityNodeCount(
    JNIEnv*, jobject) {
    rebuild_cache();
    return static_cast<jint>(g_access_nodes.size());
}

JNIEXPORT jint JNICALL
Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetNodeRole(
    JNIEnv*, jobject, jint index) {
    if (index < 0 || index >= static_cast<jint>(g_access_nodes.size())) return 0;
    return g_access_nodes[index].role;
}

JNIEXPORT jstring JNICALL
Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetNodeLabel(
    JNIEnv* env, jobject, jint index) {
    if (index < 0 || index >= static_cast<jint>(g_access_nodes.size()))
        return env->NewStringUTF("");
    return env->NewStringUTF(g_access_nodes[index].view->access_label().c_str());
}

JNIEXPORT jstring JNICALL
Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetNodeValue(
    JNIEnv* env, jobject, jint index) {
    if (index < 0 || index >= static_cast<jint>(g_access_nodes.size()))
        return env->NewStringUTF("");
    return env->NewStringUTF(g_access_nodes[index].view->access_value().c_str());
}

JNIEXPORT jfloat JNICALL
Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetNodeRangeMin(
    JNIEnv*, jobject, jint index) {
    if (index < 0 || index >= static_cast<jint>(g_access_nodes.size())) return 0.0f;
    auto* vi = dynamic_cast<pulp::view::AccessibilityValueInterface*>(
        g_access_nodes[index].view);
    return vi ? static_cast<float>(vi->get_minimum_value()) : 0.0f;
}

JNIEXPORT jfloat JNICALL
Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetNodeRangeMax(
    JNIEnv*, jobject, jint index) {
    if (index < 0 || index >= static_cast<jint>(g_access_nodes.size())) return 1.0f;
    auto* vi = dynamic_cast<pulp::view::AccessibilityValueInterface*>(
        g_access_nodes[index].view);
    return vi ? static_cast<float>(vi->get_maximum_value()) : 1.0f;
}

JNIEXPORT jint JNICALL
Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetNodeRowCount(
    JNIEnv*, jobject, jint index) {
    if (index < 0 || index >= static_cast<jint>(g_access_nodes.size())) return 0;
    auto* ti = dynamic_cast<pulp::view::AccessibilityTableInterface*>(
        g_access_nodes[index].view);
    return ti ? ti->get_row_count() : 0;
}

JNIEXPORT jint JNICALL
Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetNodeColumnCount(
    JNIEnv*, jobject, jint index) {
    if (index < 0 || index >= static_cast<jint>(g_access_nodes.size())) return 0;
    auto* ti = dynamic_cast<pulp::view::AccessibilityTableInterface*>(
        g_access_nodes[index].view);
    return ti ? ti->get_column_count() : 0;
}

JNIEXPORT void JNICALL
Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativePerformAction(
    JNIEnv*, jobject, jint action, jint node_index, jfloat value) {
    (void)value;
    View* target = nullptr;
    if (node_index >= 0 && node_index < static_cast<jint>(g_access_nodes.size())) {
        target = g_access_nodes[node_index].view;
    }
    if (!target) return;

    constexpr int ACTION_CLICK = 0;
    constexpr int ACTION_INCREMENT = 1;
    constexpr int ACTION_DECREMENT = 2;

    switch (action) {
        case ACTION_CLICK: {
            // Compute the target's centre in root coordinates and dispatch
            // through the root view. simulate_click() interprets its arg as
            // a root-relative point and then hit_tests from the receiver,
            // so it must be called on the root, not the target.
            if (!g_root_view) break;
            auto b = target->bounds();
            float cx = b.x + b.width * 0.5f;
            float cy = b.y + b.height * 0.5f;
            for (auto* p = target->parent(); p; p = p->parent()) {
                cx += p->bounds().x;
                cy += p->bounds().y;
            }
            g_root_view->simulate_click(pulp::view::Point{cx, cy});
            break;
        }
        case ACTION_INCREMENT:
            target->on_accessibility_adjust(0.05f);
            break;
        case ACTION_DECREMENT:
            target->on_accessibility_adjust(-0.05f);
            break;
        default:
            break;
    }
}

} // extern "C"

#endif // __ANDROID__
