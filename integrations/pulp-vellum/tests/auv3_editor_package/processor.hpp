#pragma once

#include <memory>

namespace pulp::format {
class Processor;
}

namespace pulp::test_fixtures {

std::unique_ptr<format::Processor> create_auv3_editor_fixture();

} // namespace pulp::test_fixtures
