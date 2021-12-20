#pragma once
#include "win64s.h"
#include "server.h"

begin_c

typedef struct client_if {
    void (*notify)(shared_memory_t* shared_memory);
    int (*start)();
    int (*stop)();
    int (*set)(const char* name, const char* value);
    const char* (*get)(const char* name);
    int (*connect)();
    int (*test)(int argc, const char* argv[]);
    int (*disconnect)();
    void (*shutdown)(); // shutdown the server (instead of disconnect)
} client_if;

extern client_if client;

// client.shutdown() is necessary when both are inside single process
// or for situation when server needs to be stopped from the outside
// (e.g. server code update)

end_c
