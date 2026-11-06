cmake_minimum_required(VERSION 3.24)
project(PulpClapIdentityConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Pulp CONFIG REQUIRED COMPONENTS host)
add_executable(clap-identity-consumer main.cpp)
target_compile_features(clap-identity-consumer PRIVATE cxx_std_20)
target_link_libraries(clap-identity-consumer PRIVATE Pulp::host)

# The positive control proves the installed header/compiler works before the
# adversarial consumers prove that public code cannot manufacture loaded evidence.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES Pulp::host)
check_cxx_source_compiles([=[
#include <pulp/host/plugin_identity.hpp>
int main() {
    auto declared = pulp::host::PluginIdentityEvidence::declared({"clap", "consumer.id"});
    return declared.verification() != pulp::host::PluginIdentityVerification::declared;
}
]=] CLAP_IDENTITY_DECLARED_CONSUMER_WORKS)
if(NOT CLAP_IDENTITY_DECLARED_CONSUMER_WORKS)
    message(FATAL_ERROR "Installed identity header positive control failed")
endif()
check_cxx_source_compiles([=[
#include <pulp/host/plugin_identity.hpp>
namespace pulp::host::detail {
class ClapIdentityAccess {
public:
    static PluginIdentityEvidence forge() {
        return {{"clap", "forged"}, PluginIdentityOrigin::clap_descriptor,
                "forged", {}, PluginIdentityVerification::loaded};
    }
};
}
int main() { auto forged = pulp::host::detail::ClapIdentityAccess::forge(); }
]=] CLAP_IDENTITY_FRIEND_REDEFINITION_COMPILES)
check_cxx_source_compiles([=[
#include <pulp/host/plugin_identity.hpp>
int main() {
    auto forged = pulp::host::detail::ClapIdentityAccess::observed({"clap", "forged"}, {}, true);
}
]=] CLAP_IDENTITY_PRIVATE_MINT_COMPILES)
if(CLAP_IDENTITY_FRIEND_REDEFINITION_COMPILES OR CLAP_IDENTITY_PRIVATE_MINT_COMPILES)
    message(FATAL_ERROR "Installed consumer can forge loaded identity evidence")
endif()
