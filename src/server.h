#pragma once
#include "win64s.h"

begin_c

typedef struct shared_data_s {
    volatile uint32_t mc; // modification count
    double timestamp;     // seconds since boot
    char data[1];
} shared_data_t;

typedef struct shared_stream_s {
    volatile int32_t position; // next data index will be written by the server
    shared_data_t frames[26];  // position == -1 before start / after stop
} shared_stream_t;

typedef struct shared_memory_s {
    volatile int32_t running; // number of client requested start() over stop()
    shared_stream_t streams[2];
} shared_memory_t;

typedef struct server_if {
    void (*notify)(); // notify all clients of new position
    int (*start)(shared_memory_t* sm);
    int (*stop)();
    int (*set)(const char* name, const char* value);
    const char* (*get)(const char* name);
    int (*main)(int argc, const char* argv[]);
    void (*shutdown)();
} server_if;

extern server_if server;

end_c
