// SceneGroup — retained container node.
//
// Item 6.1 / Pulp-native names. Owns its children by value via
// `std::unique_ptr<SceneNode>`. Mutation propagates upward via
// `SceneNode::mark_dirty()`; the group's local_bounds() is the union
// of its visible children's transformed local bounds.
#pragma once

#include <pulp/canvas/scene/scene_node.hpp>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace pulp::canvas {

class SceneGroup : public SceneNode {
public:
    SceneGroup() : SceneNode(SceneNodeKind::group) {}

    /// Take ownership of `child` and re-parent it to this group.
    /// Returns a non-owning pointer for the caller to address the child
    /// without re-walking children().
    SceneNode* add_child(std::unique_ptr<SceneNode> child) {
        if (!child) return nullptr;
        child->parent_ = this;
        SceneNode* raw = child.get();
        children_.push_back(std::move(child));
        mark_dirty();
        return raw;
    }

    /// Convenience for the common in-place construction case.
    template <class T, class... Args>
    T* emplace_child(Args&&... args) {
        auto owned = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = owned.get();
        add_child(std::move(owned));
        return raw;
    }

    /// Detach + destroy a child by index. Returns true on success.
    bool remove_child(size_t index) {
        if (index >= children_.size()) return false;
        children_[index]->parent_ = nullptr;
        children_.erase(children_.begin() + static_cast<long>(index));
        mark_dirty();
        return true;
    }

    /// Detach + destroy a child by raw pointer. Returns true on success.
    bool remove_child(SceneNode* node) {
        for (size_t i = 0; i < children_.size(); ++i) {
            if (children_[i].get() == node) {
                return remove_child(i);
            }
        }
        return false;
    }

    size_t child_count() const { return children_.size(); }
    SceneNode* child_at(size_t i) const {
        return i < children_.size() ? children_[i].get() : nullptr;
    }

    const std::vector<std::unique_ptr<SceneNode>>& children() const {
        return children_;
    }

    /// Walk the sub-tree depth-first, applying `fn(node)`. Used by
    /// `VectorScene::take_dirty_rect()` and by tests / inspectors.
    template <class Fn>
    void for_each_descendant(Fn fn) const {
        for (auto& c : children_) {
            fn(*c);
            if (c->kind() == SceneNodeKind::group) {
                static_cast<SceneGroup*>(c.get())->for_each_descendant(fn);
            }
        }
    }

    // ── SceneNode overrides ──────────────────────────────────────────────
    SceneRect local_bounds() const override;
    void paint_geometry(Canvas& canvas) const override;

private:
    std::vector<std::unique_ptr<SceneNode>> children_;
};

}  // namespace pulp::canvas
