// Planted compile negative: this internal header must never be shipped by the
// installed SDK. The receipt compiles this file separately and expects a
// nonzero compiler exit.
#include <pulp/host/signal_graph_internal.hpp>

int main() {
    return 0;
}
