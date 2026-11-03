#pragma once

// An editor factory that only carries a name, so a test can tell which UI
// package's factory a processor returned without opening an editor.

#include <pulp/format/editor_host.hpp>

#include <memory>

namespace pulp::test_fixtures {

class TaggedEditorFactory final : public format::EditorFactory {
  public:
    explicit TaggedEditorFactory(const char* tag) : tag_(tag) {}

    const char* tag() const {
        return tag_;
    }

    bool supports(format::EditorRole) const override {
        return false;
    }
    std::unique_ptr<format::Editor> create(format::Processor&, format::EditorContext&,
                                           format::EditorRole) override {
        return nullptr;
    }

  private:
    const char* tag_;
};

} // namespace pulp::test_fixtures
