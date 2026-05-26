// PulpHostBench AU — plugin registration (force-link).

#include "host_bench.hpp"

#include <pulp/format/registry.hpp>

PULP_REGISTER_PLUGIN(pulp::examples::create_host_bench_au)

extern "C" void pulp_host_bench_force_link() {}
