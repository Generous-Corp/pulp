#include "processor.hpp"

#include <pulp/format/standalone.hpp>

int main()
{
    pulp::format::StandaloneApp app(pulp::sdk_smoke::create_processor);
    return app.is_running() ? 1 : 0;
}
