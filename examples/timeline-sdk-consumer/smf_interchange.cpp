#include <pulp/smf/interchange.hpp>

int main() {
    const auto writer = pulp::smf::writer();
    return writer ? 0 : 1;
}
