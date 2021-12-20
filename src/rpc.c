#include "win64s.h"
#include <rpc.h>
#include "iface_h.h"
#include "client.h"
#include "server.h"

#pragma comment(lib, "rpcrt4.lib")

begin_c

typedef struct client_info_s {
    handle_t context; // rpc context
    handle_t notification; // event
    uint32_t client_pid;
    volatile int32_t running; // start()/stop() calls counter
} client_info_t;

const uint64_t shared_memory_size = sizeof(shared_memory_t);

static struct {
    handle_t mapping;
    thread_t cleaner;
    shared_memory_t* shared_memory;
    client_info_t clients[128];
    int32_t client_count;
    int32_t deaf_count; // number of notifications nobody listened to
    CRITICAL_SECTION cs;
    volatile bool locked;
    volatile bool shutdown;
    bool endpoint_in_use;
} s;

#define lock() do { EnterCriticalSection(&s.cs); assert(!s.locked); s.locked = true; } while (0)
#define unlock() do { assert(s.locked); s.locked = false; LeaveCriticalSection(&s.cs); } while (0)

static void create_shared_memory() {
    const uint64_t size = (shared_memory_size + 4095) / 4096 * 4096;
    fatal_if_null(s.mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, null, PAGE_READWRITE,
        (uint32_t)(size >> 32), (uint32_t)size, null));
    assert(s.mapping != null, "CreateFileMappingA() failed %s", last_error());
    s.shared_memory = MapViewOfFile(s.mapping, FILE_MAP_ALL_ACCESS, 0, (uint32_t)(size >> 32), (uint32_t)size);
    assert(s.shared_memory != null, "MapViewOfFile() failed %s", last_error());
}

static int find_client(handle_t context) {
    int ix = -1;
    for (int i = 0; i < s.client_count && ix < 0; i++) {
        if (s.clients[i].context == context) { ix = i; }
    }
    return ix;
}

static bool add_client(handle_t context, uint32_t client_pid, handle_t notification) {
    bool b = false;
    assert(0 <= s.client_count && s.client_count <= countof(s.clients));
    assert(find_client(context) < 0);
    if (s.client_count == countof(s.clients)) {
        s.client_count = countof(s.clients);
    } else {
        s.clients[s.client_count].client_pid = client_pid;
        s.clients[s.client_count].notification = notification;
        s.clients[s.client_count].context = context;
        s.clients[s.client_count].running = 0;
        s.client_count++;
        b = true;
    }
    return b;
}

static void remove_client_at(int ix) {
    assert(0 < s.client_count && s.client_count <= countof(s.clients));
    assert(0 <= ix && ix < s.client_count);
    if (ix >= 0) {
        handles.close(s.clients[ix].notification);
        s.shared_memory->running -= s.clients[ix].running;
        traceln("removing client[%d] pid=%d running=%d", ix, s.clients[ix].client_pid, s.clients[ix].running);
        s.client_count--;
        const int n = s.client_count; // last
        s.clients[ix] = s.clients[n];
        s.clients[n].client_pid = 0;
        s.clients[n].notification = null;
        s.clients[n].running = 0;
    }
    assert(0 <= s.client_count && s.client_count <= countof(s.clients));
}

static void remove_client(handle_t context) {
    bool found = false;
    int ix = find_client(context);
    if (ix >= 0) { remove_client_at(ix); }
}

static bool is_client_process_alive(uint32_t client_pid) {
    handle_t client_process = OpenProcess(PROCESS_DUP_HANDLE, false, client_pid);
    if (client_process != null) { handles.close(client_process); }
    return client_process != null;
}

static void cleanup_clients() {
    assert(0 <= s.client_count && s.client_count <= countof(s.clients));
    assert(s.shared_memory->running >= 0);
    lock();
    int32_t running = s.shared_memory->running; // before removing clients
    int i = 0;
    while (i < s.client_count) {
        if (!is_client_process_alive(s.clients[i].client_pid)) {
            remove_client_at(i);
        } else {
            i++;
        }
    }
    assert(s.shared_memory->running >= 0);
    if (running > 0 && s.shared_memory->running == 0) { server.stop(); }
    unlock();
    assert(0 <= s.client_count && s.client_count <= countof(s.clients));
}

thread_proc(cleaner, {}, { cleanup_clients(); }, {})

static void RPC_ENTRY client_disconnected(struct _RPC_ASYNC_STATE *async,
                  void* context, RPC_ASYNC_EVENT rpc_event) {
    if (rpc_event == RpcClientDisconnect) { // conext is always null
        threads.notify(&s.cleaner);
    } else {
        traceln("rpc_event=%d context=%p", rpc_event, context);
    }
}

static handle_t process_open(uint32_t pid) {
    handle_t process = null;
    fatal_if_null(process = OpenProcess(PROCESS_DUP_HANDLE, false, pid));
    return process;
}

int s_rpc_connect(handle_t context, rpc_info_t* info) {
    RPC_ASYNC_NOTIFICATION_INFO notification_info = {0};
    notification_info.NotificationRoutine = client_disconnected;
    fatal_if_not_zero(RpcServerSubscribeForNotification(context, RpcNotificationClientDisconnect, RpcNotificationTypeCallback, &notification_info));
    info->server_pid = GetCurrentProcessId();
    // server process should have the same of elevated privileges 
    // relative to client process for process open to succeed 
    handle_t client_process = process_open((uint32_t)info->client_pid);
    handle_t server_process = process_open((uint32_t)info->server_pid);
    handle_t notification = null;
    fatal_if_null(notification = handles.dup((handle_t)info->notification, client_process, server_process));
    handle_t client_mapping = null;
    fatal_if_null(client_mapping = handles.dup(s.mapping, server_process, client_process));
    info->mapping = (rpc_uint64_t)client_mapping;
    info->memory_size = shared_memory_size;
    handles.close(server_process);
    handles.close(client_process);
    lock();
    int r = add_client(context, (uint32_t)info->client_pid, notification) ?
        0 : ERROR_BLOCK_TOO_MANY_REFERENCES;
    if (r != 0) { handles.close(notification); }
    unlock();
    return r;
}

static void notify() {
    lock();
    s.deaf_count = 0;
    for (int i = 0; i < s.client_count; i++) {
        if (s.clients[i].running > 0) {
            int r = events.wait_or_timeout(s.clients[i].notification, 0);
            s.deaf_count += (r == 0); // event was not heard
            events.set(s.clients[i].notification);
        }
    }
    if (s.deaf_count > 0) { threads.notify(&s.cleaner); }
    unlock();
}

int s_rpc_start(handle_t context) {
    int r = 0;
    lock();
    int ix = find_client(context);
    assert(ix >= 0);
    if (ix < 0) {
        r = RPC_E_DISCONNECTED;
    } else if (s.clients[ix].running > 0) {
        r = SCHED_E_ALREADY_RUNNING;
    } else {
        assert(s.clients[ix].running == 0);
        s.clients[ix].running++;
        assert(s.shared_memory->running >= 0);
        s.shared_memory->running++;
        if (s.shared_memory->running == 1) { server.start(s.shared_memory); }
    }
    unlock();
    return r;
}

int s_rpc_stop(handle_t context) {
    int r = 0;
    lock();
    int ix = find_client(context);
    assert(ix >= 0);
    if (ix < 0) {
        r = RPC_E_DISCONNECTED;
    } else if (s.clients[ix].running == 0) {
        r = SCHED_E_TASK_NOT_RUNNING;
    } else {
        assert(s.clients[ix].running == 1);
        s.clients[ix].running--;
        assert(s.shared_memory->running > 0);
        s.shared_memory->running--;
        if (s.shared_memory->running == 0) { server.stop(); }
    }
    unlock();
    return r;
}

int s_rpc_set(handle_t context, unsigned char* name, unsigned char* value) {
    lock();
//  traceln("s_rpc_set(context=%p, name=\"%s\", value=\"%s\")\n", context, name, value);
    unlock();
    return 0;
}

int s_rpc_get(handle_t context, unsigned char* name, int* bytes, unsigned char** value) {
    lock();
    const char* r = "Goodbye Universe";
    *bytes = (int)strlen(r) + 1;
    *value = (char*)midl_user_allocate(*bytes);
    if (*value != null) { memcpy(*value, r, *bytes); }
//  traceln("s_rpc_get(context=%p, name=\"%s\", value=\"%s\")\n", context, name, *value);
    unlock();
    return 0;
}

int s_rpc_disconnect(handle_t context, rpc_info_t* info) {
    lock();
    remove_client(context);
    unlock();
    return 0;
}

void s_rpc_shutdown(handle_t context) {
    s.shutdown = true;
    server.shutdown();
    fatal_if_not_zero(RpcMgmtStopServerListening(null));
    fatal_if_not_zero(RpcServerUnregisterIf(s_rpc_i_v1_0_s_ifspec, null, false));
}

static int use_protocol_sequence_endpoint() {
    uint32_t r = 0;
    if (!s.endpoint_in_use) {
        r = RpcServerUseProtseqEpA("ncalrpc", RPC_C_LISTEN_MAX_CALLS_DEFAULT, "demo", null);
        assert(r == 0 || r == RPC_S_DUPLICATE_ENDPOINT, "RpcServerUseProtseqEpA() failed %s", error_to_string(r));
        s.endpoint_in_use = r == 0;
    }
    return r;
}

static int server_listen() {
    soft_realtime_thread();
    fatal_if_false(InitializeCriticalSectionAndSpinCount(&s.cs, 4096));
    create_shared_memory();
    server.notify = notify;
    threads.create(&s.cleaner, cleaner, &s);
    fatal_if_not_zero(RpcServerRegisterIf2(s_rpc_i_v1_0_s_ifspec, null, null, 
                RPC_IF_ALLOW_LOCAL_ONLY | RPC_IF_AUTOLISTEN,
                1 /* RPC_C_LISTEN_MAX_CALLS_DEFAULT */, 1024, null)); 
    // 1 call of 1KB incoming call maximum
//  https://docs.microsoft.com/en-us/windows/win32/rpc/interface-registration-flags
//  RPC_IF_AUTOLISTEN
//      This is an auto - listen interface.The run time begins listening for calls 
//      as soon as the first autolisten interface is registered, and stops listening 
//      when the last autolisten interface is unregistered.
    while (!s.shutdown) {
        fatal_if_not_zero(RpcServerListen(1, 1, true));
        fatal_if_not_zero(RpcMgmtWaitServerListen());
    }
    threads.join(&s.cleaner);
    DeleteCriticalSection(&s.cs);
    return 0;
}

int server_main(int argc, const char* argv[]) {
    uint32_t r = use_protocol_sequence_endpoint();
    assert(r == 0 || r == RPC_S_DUPLICATE_ENDPOINT, "RpcServerUseProtseqEpA() failed %s", error_to_string(r));
    if (r == 0) { r = server_listen(); }
    return r;
}

/* client */

static struct {
    HANDLE server_thread;
    thread_t notifier;
    rpc_info_t info;
    int argc;
    const char** argv;
    void* context;
    bool connected;
    bool local; // running as local service inside same process
    shared_memory_t* shared_memory;
} c;

static bool connect_to_server() {
    __try {
        int r = c_rpc_connect(c.context, &c.info);
        if (r == 0) {
            assert(c.info.server_pid != 0);
            assert(c.info.notification != 0);
            assert(c.info.mapping != 0);
            fatal_if_null(c.shared_memory = r != 0 ? null :
                MapViewOfFile((handle_t)c.info.mapping, FILE_MAP_READ, 0, 0, (size_t)c.info.memory_size));
            // handle is no longer needed after mapping succeeded
            handles.close((handle_t)c.info.mapping);
            c.info.mapping = 0; 
        }
        return r == 0;
    } __except (1) {
        uint32_t r = RpcExceptionCode();
        traceln("c_rpc_connect() failed %s", error_to_string(r));
        return false;
    }
}

static bool disconnect_from_server() {
    __try {
        c_rpc_disconnect(c.context, &c.info);
        c.connected = false;
        return true;
    } __except (1) {
        uint32_t r = RpcExceptionCode();
        traceln("c_rpc_disconnect() failed %s", error_to_string(r));
        return false;
    }
}

static uint32_t WINAPI run_server_main(void* p) { return server_listen(); }

static void start_local_server() {
    fatal_if_null(c.server_thread = CreateThread(null, 0, run_server_main, null, 0, null));
}

#define rpc_try_call(r, code) \
    __try {          \
        code         \
    } __except (1) { \
        r = (uint32_t)_exception_code(); \
        traceln("%s failed %s", #code, error_to_string(r)); \
    }

static void stop_local_server() {
    uint32_t r = 0;
    rpc_try_call(r, { c_rpc_shutdown(c.context); });
    events.wait(c.server_thread);
    handles.close(c.server_thread);
    c.connected = false;
}

thread_proc(notifier_thread_proc, {}, {
    void (*notify)() = client.notify;
    if (notify != null) {
        notify(c.shared_memory);
    }
}, {})

static void client_notify(shared_memory_t* shared_memory) {
    assert(false, "must be overriden by client");
}

static int start() { uint32_t r = 0; rpc_try_call(r, { r = c_rpc_start(c.context); }); return (int)r; }

static int stop() { uint32_t r = 0; rpc_try_call(r, { r = c_rpc_stop(c.context); }); return (int)r; }

static int set(const char* name, const char* value) {
    uint32_t r = 0;
    rpc_try_call(r, { r = c_rpc_set(c.context, (unsigned char*)name, (unsigned char*)value); });
    return (int)r;
}

static const char* get(const char* name) {
    static thread_local char val[1024];
    int bytes = 0;
    char* value = null;
    uint32_t r = 0;
    rpc_try_call(r, { r = c_rpc_get(c.context, "foo", &bytes, &value); });
    if (r == 0 && bytes > countof(val) - 1) {
        r = ERROR_INSUFFICIENT_BUFFER;
    } else {
        memcpy(val, value, bytes);
        val[bytes] = 0;
    }
    heap.free(value);
    return r == 0 ? val : "";
}

static void shutdown_sever() {
    uint32_t r = 0;
    rpc_try_call(r, { c_rpc_shutdown(c.context); });
}

static int client_connect() {
    c.local = use_protocol_sequence_endpoint() == 0;
    if (c.local) { start_local_server(); }
    memset(&c.info, 0, sizeof(c.info));
    c.info.client_pid = GetCurrentProcessId();
    c.info.notification = (rpc_uint64_t)CreateEventA(null, FALSE, FALSE, null);
    fatal_if_not_zero(RpcBindingFromStringBinding("ncalrpc:[demo]", &c_rpc_i_v1_0_c_ifspec));
    c.context = c_rpc_i_v1_0_c_ifspec;
    c.connected = connect_to_server();
    int retry = 4;
    while (!c.connected && retry > 0) {
        sleep(0.001); // yield to let server start
        c.connected = connect_to_server();
        retry--;
    }
    assert(c.connected);
    if (c.connected) {
        threads.create_with_event(&c.notifier, notifier_thread_proc, &c, (handle_t)c.info.notification);
    }
    return c.connected ? 0 : ERROR_NOT_CONNECTED;
}

static int client_disconnect() {
    if (c.connected) {
        disconnect_from_server();
    }
    threads.join(&c.notifier);
    fatal_if_false(UnmapViewOfFile(c.shared_memory));
    // stop_local_server() still needs rpc binding context to call shutdown
    if (c.local) { stop_local_server(); }
    fatal_if_not_zero(RpcBindingFree(&c.context));
    c.context = null;
    return 0;
}

int client_test(int argc, const char* argv[]);

client_if client = {
    null, // notify
    start,
    stop,
    set,
    get,
    client_connect,
    client_test,
    client_disconnect,
    shutdown_sever
};

end_c
