#include "processor.hpp"

namespace pulp::test_fixtures {

std::unique_ptr<format::Processor> create_auv3_editor_fixture() {
    return std::make_unique<Auv3EditorProcessor>();
}

} // namespace pulp::test_fixtures
