#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <memory.h>
#include <assert.h>
#define WIN32_LEAN_AND_MEAN 
#define VC_EXTRALEAN 
#include <windows.h>

// w64s stands for "Windows 64 bit Simplistic" because
// 1. Windows.h and "Win32" ALLCAPS APIs is a bit rude in 2021
//    https://en.wikipedia.org/wiki/All_caps
//    "repeated use of all caps can be considered `shouting` or irritating.
// 2. Checking error codes at all call sites for invalid parameters
//    and handling that as fatal errors better be done once.

#ifdef __cplusplus
#define begin_c extern "C" {
#define end_c }
#else
#define begin_c
#define end_c
#endif

begin_c

typedef unsigned char byte;

typedef HANDLE handle_t;

#define null ((void*)0)

#define thread_local __declspec(thread)

#define countof(a) (sizeof(a) / sizeof((a)[0]))

const char* error_to_string(uint32_t e);

const char* last_error();

void fatal_windows_error(const char* file, int line, const char* function,
                         uint32_t error, const char* call, const char* extra);

#define traceln(...) traceline(__FILE__, __LINE__, __FUNCTION__, "" __VA_ARGS__)

#undef assert // assert runtime Abort/Retry/Ignore MessageBox nonsense

#define assert(b, ...) do { \
    if (!(b)) { assertion_failed(__FILE__, __LINE__, __FUNCTION__, #b, "" __VA_ARGS__); } \
} while (0)

double seconds_since_boot();

void sleep(double seconds);

const char* timestamp_string();

typedef struct heap_i {
    void* (*alloc)(uint64_t bytes);
    void (*free)(void* p);
} heap_i;

heap_i heap;

void traceline(const char* file, int line, const char* function, const char* format, ...);

void assertion_failed(const char* file, int line, const char* function,
                      const char* condition, const char* format, ...);

#define fatal_if_false_(condition, call, error, ...) \
    do { \
        bool _b_##__LINE__ = (condition); \
        if (!_b_##__LINE__) { \
            char va_args[512]; \
            snprintf(va_args, sizeof(va_args) - 1, "" __VA_ARGS__); \
            fatal_windows_error(__FILE__, __LINE__, __FUNCTION__, error, call, va_args); \
        } \
    } while (0)

#define fatal_if_false(win32_api_call, ...) fatal_if_false_(win32_api_call, #win32_api_call, GetLastError(), __VA_ARGS__)

#define fatal_if_null(win32_api_call, ...) fatal_if_false_((win32_api_call) != null, #win32_api_call, GetLastError(), __VA_ARGS__)

#define fatal_if_not_zero(win32_api_call, ...) \
    do { \
        uint32_t _r_##__LINE__ = (win32_api_call); \
        fatal_if_false_(_r_##__LINE__ == 0, #win32_api_call, _r_##__LINE__, __VA_ARGS__); \
    } while (0)

typedef struct thread_s {
    void* that;
    HANDLE events[2];
    HANDLE thread;
} thread_t;


#define soft_realtime_thread() {                                                            \
    fatal_if_false(SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS));         \
    fatal_if_false(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL));   \
}

#define thread_begin(p)             \
    thread_t* self = (thread_t*)p;   \
    void* that = self->that;         \

#define thread_wait(_timeout_ms_)                                                           \
        uint32_t _r_##__LINE__ = events.wait_any_or_timeout(2, self->events, _timeout_ms_); \
        if (_r_##__LINE__ == 0) { break; }                                                  \

#define thread_end                                                                          \
    return 0;                                                                               \

#define thread_proc(proc, prelude, code, coda)                                              \
                                                                                            \
static uint32_t WINAPI proc(void* _thread_) {                                               \
    thread_begin(_thread_)                                                                  \
    prelude                                                                                 \
    for (;;) {                                                                              \
        thread_wait(INFINITE)                                                               \
        code                                                                                \
    }                                                                                       \
    coda                                                                                    \
    thread_end                                                                              \
} 

typedef struct {
    void (*close)(handle_t h);
} handles_if;

extern handles_if handles;

typedef struct {
    handle_t (*create)();
    handle_t (*create_manual)();
    void (*set)(handle_t e);
    void (*reset)(handle_t e);
    void (*wait)(handle_t e);
    int (*wait_or_timeout)(handle_t e, uint32_t milliseconds); // 0 or -1 on timeout
    int (*wait_any)(int n, handle_t events[]); // -1 on abandon
    int (*wait_any_or_timeout)(int n, handle_t e[], uint32_t milliseconds); // -1 on timeout or abandon
    void (*dispose)(handle_t e);
} events_if;

extern events_if events;

typedef struct {
    void (*create_with_event)(thread_t* thread, uint32_t (WINAPI *proc)(void* thread), 
                              void* that, handle_t e);
    void (*create)(thread_t* thread, uint32_t (WINAPI *proc)(void* thread), void* that);
    void (*notify)(thread_t* thread);
    void (*join)(thread_t* thread); // closes ALL handles
} threads_if;

extern threads_if threads;

end_c
