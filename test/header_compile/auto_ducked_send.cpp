#include <pulp/signal/auto_ducked_send.hpp>

void auto_ducked_send_header_self_containment() {
    pulp::signal::AutoDuckedSend send;
    (void)send.configure({});
    (void)send.prepare(48000.0f);
    (void)send.process(0.0f, 0.0f, 0.0f, 0.0f);
}
