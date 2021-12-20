#include "win64s.h"
#include "server.h"

begin_c

static volatile shared_memory_t* sm;
static thread_t test;
static volatile bool running;
extern bool verbose;

static uint32_t WINAPI test_thread_proc(void* p) {
    soft_realtime_thread();
    thread_begin(p)
    double start_time = seconds_since_boot();
    int k = 0;
    for (;;) {
        thread_wait_or_break(1000);
        // check if there are clients that requested streams to be running:
        if (sm->running > 0) {
            for (int i = 0; i < countof(sm->streams); i++) {
                volatile shared_stream_t* st = &sm->streams[i];
                // stream[0] is 1Hz stream, stream[1] is 2Hz stream
                if (k % (i + 1) == 0) {
                    int ix = st->position < 0 ? 0 : st->position;
                    char base = rand() > RAND_MAX / 2 ? 'a' : 'A';
                    byte data = (byte)((rand() % 26) + base);
                    st->frames[ix].data[0] = data;
                    st->frames[ix].timestamp = seconds_since_boot();
                    st->frames[ix].mc++;
                    st->position = (ix + 1) % countof(st->frames);
                    server.notify();
                    // uncommenting trace below severely affects latency measurements
                    if (verbose) {
                        traceln("%s %6.3f stream[%d].frames[%02d].data:= 0x%02X '%c' (mc=%d)",
                            timestamp_string(), st->frames[ix].timestamp - start_time,
                            i, ix, data, data, st->frames[ix].mc);
                    }
                }
            }
        }
        k++;
    }
    thread_end
}

static int start(shared_memory_t* m) {
    // called when shared_memory.running has been changed to none zero
    if (sm == null) {
        sm = m;
        for (int i = 0; i < countof(sm->streams); i++) { sm->streams[i].position = -1; }
    } else {
        assert(sm == m, "change in shared memory location is not supported yet");
    }
    if (test.thread == null) {
        threads.create(&test, test_thread_proc, null);
    }
    threads.notify(&test);
    traceln("-- started");
    return 0;
}

static int stop() { 
    // called when shared_memory.running has been changed to zero
    for (int i = 0; i < countof(sm->streams); i++) { sm->streams[i].position = -1; }
    threads.notify(&test); 
    traceln("-- stopped");
    return 0;
}

static int set(const char* name, const char* value) {
    return 0;
}

static const char* get(const char* name) {
    return 0;
}

static void server_shutdown() {
    if (test.thread != null) { threads.join(&test); }
}

int server_main(int argc, const char* argv[]);

server_if server = {
    null, // notify
    start,
    stop,
    set,
    get,
    server_main,
    server_shutdown
};

end_c
