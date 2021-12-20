#include "win64s.h"
#include "client.h"
#include "server.h"

begin_c

static volatile shared_memory_t* sm;
static volatile handle_t notification;
extern bool verbose;

static void notify(shared_memory_t* m) {
    assert(sm == null || sm == m);
    sm = m;
    if (notification != null) { events.set(notification); }
}

static void roundtrip() {
    enum { N = 100000 };
    double time = seconds_since_boot();
    for (int i = 0; i < N; i++) {
        // 10 microseconds for local and 20 microseconds for remote call
        fatal_if_not_zero(client.set("foo", "bar"));
    }
    time = seconds_since_boot() - time;
    traceln("client.set() %.3f microseconds\n", time * 1000000.0 / N);
    traceln("client.get(\"foo\")=\"%s\"\n", client.get("Hello World"));
}

static void streaming() {
    double start_time = seconds_since_boot();
    fatal_if_not_zero(client.start());
    double max_latency[countof(sm->streams)] = {0};
    int32_t position[countof(sm->streams)];
    for (int i = 0; i < countof(position); i++) { position[i] = -1; }
    for (int k = 0; k < 27; k++) {
        int r = events.wait_or_timeout(notification, 3000);
        if (r != 0) {
            traceln("TIMEOUT: server is probably dead");
            exit(1);
        }
        for (int i = 0; i < countof(sm->streams); i++) {
            volatile shared_stream_t* st = &sm->streams[i];
            if (st->position >= 0) {
                int ix = (st->position + countof(st->frames) - 1) % countof(st->frames);
                if (ix != position[i]) {
                    uint32_t mc_before = st->frames[ix].mc;
                    byte data = st->frames[ix].data[0];
                    uint32_t mc_after = st->frames[ix].mc;
                    position[i] = ix;
                    if (mc_before != mc_after) {
                        traceln("%6.3f IGNORE stream[%d].frames[%d] because it was modified in-flight",
                            st->frames[ix].timestamp - start_time, i, ix);
                    } else {
                        double latency = (seconds_since_boot() - st->frames[ix].timestamp) * 1000 * 1000;
                        if (latency < 1000 * 1000 && latency > max_latency[i]) {
                            max_latency[i] = latency;
                        } else {
                            // latency greater then a second happens when a client connects to
                            // already running service
                        }
                        if (verbose) {
                            traceln("%6.3f stream[%d].frames[%02d].data = 0x%02X '%c' (mc=%d) latency=%.3fus",
                                st->frames[ix].timestamp - start_time, i, ix, data, data, mc_after, latency);
                        }
                    }
                }
            }
        }
    }
    fatal_if_not_zero(client.stop());
    for (int i = 0; i < countof(position); i++) {
        traceln("latency[%d]=%.1f us", i, max_latency[i]);
    }
    // observed max latency upto 200 microseconds
}

int client_test(int argc, const char* argv[]) {
    soft_realtime_thread();
    roundtrip();
    notification = events.create();
    client.notify = notify;
    streaming();
    client.notify = null; // no more calls to client notify past this point
    handle_t n = notification;
    notification = null;
    events.dispose(n);
    return 0;
}

end_c
