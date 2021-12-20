#include "win64s.h"
#include <rpc.h>

begin_c

static void vtraceln(const char* file, int line, const char* function, const char* format, va_list vl) {
    char s[2048];
    s[0] = 0;
    char* sb = s;
    int left = sizeof(s) - 1;
    if (file != null && line >= 0) {
        int n = snprintf(sb, left, "%s(%d): [%05d] %s ", file, line, GetCurrentThreadId(), function);
        sb += n;
        left -= n;
    }
    int k = vsnprintf(sb, left, format, vl);
    sb += k;
    if (sb[-1] != '\n') {
        *sb = '\n';
        sb++;
        *sb = 0;
    }
    OutputDebugStringA(s);
    fprintf(stderr, "%s", s);
}

void traceline(const char* file, int line, const char* function, const char* format, ...) {
    va_list vl;
    va_start(vl, format);
    vtraceln(file, line, function, format, vl);
    va_end(vl);
}

static const char* assertion_failed_filename(const char* file) {
    char* fn = strrchr(file, '/');
    if (fn == (void*)0) { fn = strrchr(file, '\\'); }
    return fn != (void*)0 ? fn + 1 : file;
}

#define breakpoint() do { if (IsDebuggerPresent()) { DebugBreak(); } } while (0)

void assertion_failed(const char* file, int line, const char* function,
                      const char* condition, const char* format, ...) {
    const char* fn = assertion_failed_filename(file);
    const int n = (int)strlen(format);
    if (n == 0) {
        traceline(file, line, function, "assertion failed: \"%s\"", condition);
    } else {
        traceline(file, line, function, "assertion failed: \"%s\"", condition);
        va_list va;
        va_start(va, format);
        vfprintf(stderr, format, va);
        vtraceln(file, line, function, format, va);
        va_end(va);
    }
    breakpoint();
    exit(1);
}

static void* allocate(uint64_t bytes) {
    void* p = malloc(bytes);
//  traceln("malloc(%p[%d])\n", p, (int)bytes);
    return p;
}

static void deallocate(void* p) {
//  traceln("free(%p)\n", p);
    free(p);
}

heap_i heap = { allocate, deallocate };

void* __RPC_USER MIDL_user_allocate(size_t bytes) { return heap.alloc(bytes); }
void  __RPC_USER MIDL_user_free(void* p) { heap.free(p); }

double seconds_since_boot() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    static double one_over_freq;
    if (one_over_freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        one_over_freq = 1.0 / f.QuadPart;
    }
    return (double)li.QuadPart * one_over_freq;
}

void sleep(double seconds) {
    assert(seconds >= 0);
    if (seconds < 0) { seconds = 0; }
    int64_t ns100 = (int64_t)(seconds * 1.0e+7); // in 0.1 microsecond aka 100ns
    typedef int (__stdcall *nt_delay_execution_t)(BOOLEAN Alertable, PLARGE_INTEGER DelayInterval);
    static nt_delay_execution_t NtDelayExecution;
    // delay in 100-ns units. negative value means delay relative to current.
    LARGE_INTEGER delay; // delay in 100-ns units.
    delay.QuadPart = -ns100; // negative value means delay relative to current.
    if (NtDelayExecution == null) {
        HMODULE ntdll = LoadLibraryA("ntdll.dll");
        assert(ntdll != null);
        if (ntdll != null) {
            NtDelayExecution = (nt_delay_execution_t)GetProcAddress(ntdll, "NtDelayExecution");
        }
    }
    assert(NtDelayExecution != null);
    if (NtDelayExecution != null) {
        NtDelayExecution(false, &delay); //  If "alertable" is set, execution can break in a result of NtAlertThread call.
    } else {
        ExitProcess(ERROR_FATAL_APP_EXIT);
    }
}

const char* timestamp_string() {
    FILETIME time;
    GetSystemTimePreciseAsFileTime(&time);
    uint64_t ns100 = *(uint64_t*)&time; // 0.1 microseconds resolution
    static char thread_local text[128];
    int64_t us = ns100 / 10; // microseconds
    int64_t s = (int64_t)(us / (1000 * 1000)); // seconds
    snprintf(text, countof(text) - 1, "%02d:%02d.%03d:%03d",
        (int)(s / 60) % 60, (int)(s % 60), (int)(us / 1000) % 1000, (int)(us % 1000));
    return text;
}

void fatal_windows_error(const char* file, int line, const char* function,
                         uint32_t error, const char* call, const char* extra) {
    fprintf(stderr, "%s failed %s %s", call, error_to_string(error), extra);
    traceline(file, line, function, "%s failed %s %s", call, error_to_string(error), extra);
    breakpoint();
    exit(1);
}

const char* error_to_string(uint32_t e) {
    const DWORD neutral = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);
    const DWORD format = FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS;
    static char thread_local s[512];
    HRESULT hr = 0 <= e && e <= 0xFFFF ? HRESULT_FROM_WIN32(e) : e;
    int n = FormatMessageA(format, null, hr, neutral, s, sizeof(s) - 1, null);
    s[sizeof(s) - 1] = 0;
    int k = (int)strlen(s);
    if (k > 0 && s[k - 1] == '\n') { s[k - 1] = 0; }
    k = (int)strlen(s);
    if (k > 0 && s[k - 1] == '\r') { s[k - 1] = 0; }
    static char thread_local m[1024];
    snprintf(m, sizeof(m) - 1, "0x%08X(%d) \"%s\"", e, e, s);
    return m;
}

const char* last_error() { return error_to_string(GetLastError()); }

static void handle_close(handle_t handle) {
    fatal_if_false(CloseHandle(handle));
}

handles_if handles = {
    handle_close
};

static handle_t events_create() {
    handle_t e = null;
    fatal_if_null(e = CreateEvent(null, false, false, null));
    return e;
}

static handle_t events_create_manual() {
    handle_t e = null;
    fatal_if_null(e = CreateEvent(null, true, false, null));
    return e;
}

static void events_set(handle_t e) { fatal_if_false(SetEvent(e)); }

static void events_reset(handle_t e) { fatal_if_false(ResetEvent(e)); }

static void event_dispose(handle_t e) { handles.close(e); }

static int events_wait_or_timeout(handle_t e, uint32_t ms) {
    uint32_t r = 0;
    fatal_if_false((r = WaitForSingleObject(e, ms)) != WAIT_FAILED);
    return r == WAIT_OBJECT_0 ? 0 : -1; // all WAIT_ABANDONED as -1  
}

static void events_wait(handle_t e) { events_wait_or_timeout(e, INFINITE); }

static int events_wait_any_or_timeout(int n, handle_t events[], uint32_t ms) {
    uint32_t r = 0;
    fatal_if_false((r = WaitForMultipleObjects(n, events, false, ms)) != WAIT_FAILED);
    // all WAIT_ABANDONED_0 and WAIT_IO_COMPLETION 0xC0 as -1  
    return WAIT_OBJECT_0 <= r && r < WAIT_OBJECT_0 + n ? r - WAIT_OBJECT_0 : -1;
}

static int events_wait_any(int n, handle_t e[]) { 
    return events_wait_any_or_timeout(n, e, INFINITE);
}

events_if events = {
    events_create,
    events_create_manual,
    events_set,
    events_reset,
    events_wait,
    events_wait_or_timeout,
    events_wait_any,
    events_wait_any_or_timeout,
    handle_close
};

static void threads_create_with_event(thread_t* thread, uint32_t(*proc)(void* thread), void* that, handle_t e) {
    assert(thread->events[0] == null);
    assert(thread->events[1] == null);
    assert(thread->thread == null);
    assert(thread->that == null);
    thread->that = that;
    fatal_if_null(thread->events[0] = CreateEvent(null, false, false, null));
    fatal_if_null(thread->events[1] = e);
    fatal_if_null(thread->thread = CreateThread(null, 0, proc, thread, 0, null));
}

static void threads_create(thread_t* thread, uint32_t(*proc)(void* thread), void* that) {
    threads_create_with_event(thread, proc, that, CreateEvent(null, false, false, null));
}

static void threads_join(thread_t* thread) {
    fatal_if_false(SetEvent(thread->events[0]));
    int r = 0;
    fatal_if_false((r = WaitForSingleObject(thread->thread, INFINITE)) == WAIT_OBJECT_0);
    events.dispose(thread->events[0]);
    events.dispose(thread->events[1]);
    handles.close(thread->thread);
    memset(thread, 0, sizeof(*thread));
}

static void threads_notify(thread_t* thread) {
    fatal_if_false(SetEvent(thread->events[1]));
}

threads_if threads = {
    threads_create_with_event,
    threads_create,
    threads_notify,
    threads_join
};

end_c
