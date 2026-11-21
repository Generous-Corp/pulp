// A shared object that references a function nothing defines. It links because
// a module may leave symbols undefined, and `ldd -r` reports the reference; the
// no-unresolved-symbols check runs against it as its negative control.
extern "C" int pulp_probe_undefined_function();

extern "C" int pulp_probe_entry() {
    return pulp_probe_undefined_function();
}
