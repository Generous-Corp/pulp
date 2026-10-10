// A stand-in runtime dependency for the installed-host closure E2E: a real
// signed Mach-O library staged beside the host, so the closure carriage is
// exercised without depending on which GPU runtime the renderer links.
extern "C" int pulp_control_e2e_runtime_dependency() {
    return 1;
}
