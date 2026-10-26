// A loadable module with one local (non-exported) function, so a test can tell
// a module that kept its local symbols from one that had them stripped.

namespace {

__attribute__((noinline, used)) int pulp_ship_strip_fixture_local(int x) {
    return x * 3 + 1;
}

} // namespace

extern "C" __attribute__((visibility("default"))) int pulp_ship_strip_fixture_entry(int x) {
    return pulp_ship_strip_fixture_local(x);
}
