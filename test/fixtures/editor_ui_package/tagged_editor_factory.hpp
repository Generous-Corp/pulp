#pragma once

// An editor factory that carries a name, so a test can tell which UI
// package's factory a processor returned, and builds a minimal editor, so a
// host can open the plug-in's GUI through the contract.

#include <pulp/format/editor_host.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace pulp::test_fixtures {

/// Appends `event` to the file PULP_TEST_EDITOR_LOG names, when set, so a
/// host process can see what an editor inside a loaded module did.
inline void log_editor_event(const char* event) {
    if (const char* path = std::getenv("PULP_TEST_EDITOR_LOG")) {
        if (std::FILE* f = std::fopen(path, "a")) {
            std::fprintf(f, "%s\n", event);
            std::fclose(f);
        }
    }
}

/// Attaches to any window and keeps the size the host gives it.
class TaggedEditor final : public format::Editor {
  public:
    bool supports_parent(format::NativeParentKind kind) const override {
        return kind != format::NativeParentKind::none;
    }
    bool attach(const format::NativeParent&) override {
        return true;
    }
    void detach() override {
        log_editor_event("detach");
    }
    void invalidate() override {
        log_editor_event("invalidate");
    }
    format::EditorSize size() const override {
        return size_;
    }
    bool set_size(const format::EditorSize& size) override {
        size_ = size;
        return true;
    }

  private:
    format::EditorSize size_{320, 200, 1.0};
};

class TaggedEditorFactory final : public format::EditorFactory {
  public:
    explicit TaggedEditorFactory(const char* tag) : tag_(tag) {}

    const char* tag() const {
        return tag_;
    }

    bool supports(format::EditorRole role) const override {
        return role == format::EditorRole::editor;
    }
    std::unique_ptr<format::Editor> create(format::Processor&, format::EditorContext&,
                                           format::EditorRole role) override {
        if (role != format::EditorRole::editor)
            return nullptr;
        return std::make_unique<TaggedEditor>();
    }

  private:
    const char* tag_;
};

} // namespace pulp::test_fixtures
