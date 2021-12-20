#include "win64s.h"
#include "server.h"
#include "client.h"

bool verbose; // very global

static bool option(int argc, const char* argv[], const char* name) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) { return true; }
    }
    return false;
}

int main(int argc, const char* argv[]) {
    int r = 0;
    bool shutdown_when_done = option(argc, argv, "--shutdown");
    verbose = option(argc, argv, "--verbose") || option(argc, argv, "-v");
    if (argc > 1 && strstr(argv[1], "server") != null) {
        r = server.main(argc, argv);
    } else if (argc > 1 && strstr(argv[1], "client") != null) {
        r = client.connect();
        if (r == 0) {
            r = client.test(argc, argv);
            if (shutdown_when_done) {
                client.shutdown();
            } else {
                r = client.disconnect();
            }
        }
    } else {
        traceln("rpc server|client [--shutdown] [-v] [--verbose]");
        r = 1;
    }
    if (r != 0) {
        traceln("error: %s", error_to_string(r));
    }
    return r;
}

end_c
