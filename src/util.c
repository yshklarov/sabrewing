#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
// We need _POSIX_C_SOURCE 199309L so that certain time functions are defined.
#define _POSIX_C_SOURCE 199309L
#endif
#endif

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
//#include <tgmath.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN    // Exclude rarely-used definitions.
  #include <windows.h>
  #include <io.h>
  #include <memoryapi.h>
  #include <synchapi.h>
  #define F_OK 0
  #define access _access
  #include <intrin.h>
#else
  #include <errno.h>
  #include <sys/mman.h>
  #include <sys/time.h>
  #include <time.h>
  #include <unistd.h>
  #include <emmintrin.h>
  #include <x86intrin.h>
#endif


/**************** Types ****************/

typedef unsigned char byte;
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;
typedef float     f32;
typedef double    f64;
typedef ptrdiff_t isize;
typedef size_t    usize;

// X Macros
#define FOR_BASIC_TYPES \
    X(byte)\
    X(u8)  \
    X(u16) \
    X(u32) \
    X(u64) \
    X(i8)  \
    X(i16) \
    X(i32) \
    X(i64) \
    X(isize) \
    X(usize) \

#define FOR_INTEGER_TYPES \
    X(i32) \
    X(u32) \
    X(i64) \
    X(u64) \

#ifndef I32_MAX
  #define I32_MAX 0x7FFFFFFF
#endif
#ifndef U32_MAX
  #define U32_MAX 0xFFFFFFFFu
#endif
#ifndef I64_MAX
  #define I64_MAX 0x7FFFFFFFFFFFFFFF
#endif
#ifndef U64_MAX
  #define U64_MAX 0xFFFFFFFFFFFFFFFFu
#endif

#define X(T) \
typedef struct { T lower; T upper; T stride; } range_##T;
FOR_INTEGER_TYPES
#undef X

typedef struct
{
    u16 year; // e.g., 2025
    u8 month; // 1 through 12
    u8 day; // 1 through 31
    u8 weekday; // 0 is Sunday; 6 is Saturday
    u8 hour; // 0 through 23
    u8 minute; // 0 through 59
    u8 second; // 0 through 59
    u16 millisecond; // 0 through 999
} timedate;


/**************** Forward declarations ****************/

u64 get_ostime_count(bool pause_for_rollover);
u64 get_ostime_freq();
u64 get_ostime_ms();
timedate get_timedate();


/**************** Utility macros & functions ****************/

// TODO I'm not sure that I like asserts -- probably better to crash (abort() or exit()).
#ifndef assertm
  #define assertm(exp, msg) assert(((void)(msg), (exp)))
#endif

#if defined(_MSC_VER)
  #define NEVER_INLINE __declspec(noinline)  // MSVC
#elif defined(__GNUC__) || defined(__clang__)
  #define NEVER_INLINE __attribute__((noinline))  // GCC/Clang
#else
  #define NEVER_INLINE  // Fallback for unknown compilers
#endif

// Size of a static array.
#define ARRAY_SIZE(_ARR) ((usize)(sizeof(_ARR) / sizeof(*(_ARR))))

#define SWAP_u32(a,b) u32 _SWAP_TMP = (a); (a) = (b); (b) = _SWAP_TMP
#define SWAP_f64(a,b) f64 _SWAP_TMP = (a); (a) = (b); (b) = _SWAP_TMP

#ifndef MIN
  #define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
  #define MAX(a,b) (((a) < (b)) ? (b) : (a))
#endif

#define CLAMP(x, lower, upper) (((x) < (lower)) ? (lower) : (((x) > (upper)) ? (upper) : (x)))
u8 clamp_i64_u8(i64 x) { return (u8)CLAMP(x, 0, 0xFF); }
u16 clamp_i32_u16(i32 x) { return (u16)CLAMP(x, 0, 0xFFFF); }
u16 clamp_i64_u16(i64 x) { return (u16)CLAMP(x, 0, 0xFFFF); }
u16 clamp_usize_u16(usize x) { return (u16)MIN(x, 0xFFFF); }
u32 clamp_i32_u32(i32 x) { return (u32)MAX(0, x); }
u32 clamp_i64_u32(i64 x) { return (u32)MAX(0, x); }
i32 clamp_u64_i32(u64 x) { return (x > I32_MAX) ? I32_MAX : (i32)x; }

#define X(T) \
T range_##T##_count(range_##T r) { return (r.upper - r.lower) / r.stride + 1; } \
void range_##T##_repair(range_##T * r)                                  \
{                                                                       \
    r->lower = MAX(0, r->lower);                                        \
    r->upper = MAX(0, r->upper);                                        \
    r->stride = MAX(1, r->stride);                                      \
    if (r->lower > r->upper) {                                          \
        r->upper = r->lower;                                            \
    }                                                                   \
}
FOR_INTEGER_TYPES
#undef X

// Macro user must provide an extra enclosing block scope. Arguments must be individual keywords.
#define loop_over_range_i32(_r, _n, _n_idx)     \
    u64 _n_idx = 0;                             \
    i32 _n;                                     \
    u64 _n_count = (u64)range_i32_count(_r);    \
    for (_n_idx = 0, _n = _r.lower;             \
         _n_idx < _n_count;                     \
         ++n_idx, n += _r.stride)

// x must be unsigned; k must satisfy 0 <= k < 32
#define ROT32(x,k) (((x)<<(k))|((x)>>(32-(k))))
// x must be unsigned; k must satisfy 0 <= k < 64
#define ROT64(x,k) (((x)<<(k))|((x)>>(64-(k))))

// Reverse an array in place.
void reverse_u32(u64 n, u32* data)
{
    for (u64 i = 0; i < n/2; ++i) {
        SWAP_u32(data[i], data[n-1-i]);
    }
}


// TODO Standard mathematical div and mod operators (can copy from stb_divide.h)


// Repair a damaged max heap by sifting the given element down to its correct place.
void util_siftdown(f64* data, u32 siftee, u32 end)
{
    f64 data_siftee = data[siftee];
    for (;;) {
        u32 target = 2*siftee + 1;  // Left child of siftee.
        if (target >= end) {
            break;
        }
        if ((target + 1 < end) && (data[target] < data[target+1])) {
            // The right child is larger, so sift rightwards instead.
            ++target;
        }
        if (data_siftee < data[target]) {
            // Sift down the tree by one level. No need to write into the child; it will be
            // written during the next iteration.
            data[siftee] = data[target];
            siftee = target;
        } else {
            // Done sifting; this is the lowest it will go.
            break;
        }
    }
    data[siftee] = data_siftee;
}

// Rearrange the elements of the given array into a max heap. In-place.
void util_maxheap(f64* data, u32 n)
{
    if (n < 2) return;
    // Begin with the parent of the last element in the heap.
    for (u32 siftee = n/2; siftee > 0; ) {
        util_siftdown(data, --siftee, n);
    }
}

// Heapsort.
void util_sort(f64* data, u32 n)
{
    if (n < 2) return;
    util_maxheap(data, n);
    do {
        --n;
        SWAP_f64(data[0], data[n]);
        util_siftdown(data, 0, n);
    } while (n > 1);
}


/**************** Random numbers ****************/

// JSF (Jenkins Small Fast) random number generator
// https://burtleburtle.net/bob/rand/smallprng.html

typedef struct { u64 a; u64 b; u64 c; u64 d; } rand_state;
static rand_state rand_state_global;

// Get random data, and increment the seed.
u64 rand_raw(rand_state* x)
{
    u64 e = x->a - ROT32(x->b, 27);
    x->a = x->b ^ ROT32(x->c, 17);
    x->b = x->c + x->d;
    x->c = x->d + e;
    x->d = e + x->a;
    return x->d;
}

void rand_init_from_seed(rand_state* x, u64 seed)
{
    x->a = 0xf1ea5eed;
    x->b = x->c = x->d = seed;
    for (u64 i = 0; i < 20; ++i) {
        rand_raw(x);
    }
}

// Get a seed that can be fed into a random number generator. Two successive calls may
// return the same seed if they are too close together, but the time resolution is
// very fine on most platforms (less than 1 microsecond).
u64 rand_get_seed_from_time()
{
    return get_ostime_count(false);
}

void rand_init_from_time(rand_state* x)
{
    rand_init_from_seed(x, rand_get_seed_from_time());
}

i32 rand_i32(rand_state* x) { return (i32)rand_raw(x); }
i64 rand_i64(rand_state* x) { return (i64)rand_raw(x); }
u32 rand_u32(rand_state* x) { return (u32)rand_raw(x); }
u64 rand_u64(rand_state* x) { return (u64)rand_raw(x); }
u64 randg_i32() { return (i32)rand_raw(&rand_state_global); }

// Generate a uniform random integer from the closed interval [min, max].
//
// Parameters:
//   min <= max.
u32 rand_range_unif(rand_state* x, u32 min, u32 max)
{
    assertm(min <= max, "Cannot sample from empty range.");
    // For uniformity, it's necessary that maximum delta (2^32 - 1) be much smaller than the
    // maximum value of rand_u64() (2^64 - 1).
    // TODO This is a slow (and biased) algorithm; instead, implement Daniel Lemire's method.
    u32 raw = (u32)(rand_u64(x) % ((u64)max - (u64)min + 1));
    return min + raw;
}

// This is very inefficient; call rand_u64() to get 64 bits all at once.
u32 rand_bool(rand_state* x)
{
    return rand_u64(x) % 2;
}

// Return `true` with probability p.
bool rand_bernoulli(rand_state* x, f32 p)
{
    if (p == 0.0f) return false;
    if (p == 1.0f) return true;
    return ((f32)rand_u64(x) / (f32)U64_MAX) < p;
}



// Randomly pick a combination uniformly from the (n choose k) possibilities. Store the result in
// combination. Implements Robert Floyd's algorithm.
//
// Parameters:
//    n, k: Will choose k random elements of combination to be true, and n - k to be false.
//    combination: Must point to contiguous array of n bools.
void rand_combination(rand_state* x, u32 n, u32 k, bool combination[])
{
    assert(n >= k);
    for (u32 i = 0; i < n; ++i) {
        combination[i] = false;
    }
    for (u32 j = n - k; j < n; ++j) {
        u32 r = rand_range_unif(x, 0, j);
        if (combination[r]) {
            combination[j] = true;
        } else {
            combination[r] = true;
        }
    }
    /*
    // DEBUG
    std::cerr << "Selected random combination (" << n << ", " << k << "): ";
    for (int i {0}; i < n; ++i) {
        std::cerr << (combination[i] ? "1" : "0") << " ";
    }
    std::cerr << std::endl;
    */
}


/**************** File I/O ****************/

bool file_exists(char const * filename)
{
    return access(filename, F_OK) == 0;
}


/**************** Time ****************/

void sleep_ms(u32 milliseconds)
{
  #ifdef _WIN32
    Sleep(milliseconds);
  #else
    // Unfortunately, the more convenient usleep() has been deprecated.
    struct timespec ts = {0};
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    while ((nanosleep(&ts, &ts) == -1) && errno == EINTR);
  #endif
}

#ifdef _WIN32
u64 get_ostime_count(bool pause_for_rollover)
{
    LARGE_INTEGER qpc_now;
    QueryPerformanceCounter(&qpc_now);
    if (pause_for_rollover) {
        LARGE_INTEGER qpc_prev = qpc_now;
        do {
            QueryPerformanceCounter(&qpc_now);
        } while (qpc_prev.QuadPart == qpc_now.QuadPart);
    }
    return qpc_now.QuadPart;
}

u64 get_ostime_freq()
{
    // The QPC frequency is guaranteed to never change while the system is running.
    static LARGE_INTEGER qpc_frequency = {0};
    if (!qpc_frequency.QuadPart) {
        QueryPerformanceFrequency(&qpc_frequency);
    }
    return qpc_frequency.QuadPart;
}

#else
u64 get_ostime_count(bool pause_for_rollover)
{
    struct timespec ts_now = {0};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts_now);
    if (pause_for_rollover) {
        struct timespec ts_prev = {0};
        do {
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts_now);
        } while ((ts_now.tv_sec == ts_prev.tv_sec) && (ts_now.tv_nsec == ts_prev.tv_nsec));
    }
    return ((u64)ts_now.tv_sec * 1000000000) + (u64)ts_now.tv_nsec;
}

u64 get_ostime_freq()
{
    return 1000000000;
}
#endif

// Obtain a monotonically-increasing timestamp, in milliseconds. This is for measurements only -- do
// not use the return value for human-readable timestamps.
u64 get_ostime_ms()
{
    // This is about as high as we can go without risking integer overflow -- don't try to
    // do the same thing for microseconds.
    return (u64)(1000ll * get_ostime_count(false) / get_ostime_freq());
}

// Get the time and date in the local timzone.
timedate get_timedate()
{
    timedate now;
  #ifdef _WIN32
    SYSTEMTIME now_win32;
    GetLocalTime(&now_win32);
    now.year = (u16)now_win32.wYear;
    now.month = (u8)now_win32.wMonth;
    now.day = (u8)now_win32.wDay;
    now.weekday = (u8)now_win32.wDayOfWeek;
    now.hour = (u8)now_win32.wHour;
    now.minute = (u8)now_win32.wMinute;
    now.second = (u8)now_win32.wSecond;
    now.millisecond = (u16)now_win32.wMilliseconds;
  #else
    // WARNING: Not thread-safe.
    struct timespec now_linux = {0};
    struct tm tm_now = {0};
    // Possible race conditon: Changed timezone/etc. Unfortunately, there doesn't seem to be a
    // simple atomic way to get the time together with milliseconds.
    clock_gettime(CLOCK_REALTIME, &now_linux);
    struct tm* tm_now_tmp = localtime(&now_linux.tv_sec);
    tm_now = *tm_now_tmp;  // Avoid race condition: other threads might call localtime().
    now.year = (u16)(tm_now.tm_year + 1900);
    now.month = (u8)(tm_now.tm_mon + 1);
    now.day = (u8)tm_now.tm_mday;
    now.weekday = (u8)tm_now.tm_wday;
    now.hour = (u8)tm_now.tm_hour;
    now.minute = (u8)tm_now.tm_min;
    now.second = (u8)tm_now.tm_sec;
    now.millisecond = (u16)(now_linux.tv_nsec / 1000000);
  #endif
    return now;
}

// Format the time and date as a null-terminated string in human-readable form.
// E.g., [2000-01-01 08:12:34] (needs buf_len >= 19+2+1 = 22; otherwise, will truncate).
// The square brackets will be present if `bracketed` is true.
// The length `buf_len` should include the position for the null terminator.
//
// Return the number of characters printed, *not* including the null terminator.
//
#define TIMEDATE_FMT_LEN 19
u32 print_timedate(u8* buf, u32 buf_len, timedate td, bool bracketed)
{
    char const* format_str = bracketed
        ? "[%04u-%02u-%02u %02u:%02u:%02u]"
        :  "%04u-%02u-%02u %02u:%02u:%02u";
    return (u32)snprintf((char*)buf, buf_len,
             format_str,
             td.year, td.month, td.day, td.hour, td.minute, td.second);
}


/**************** Memory management ****************/

// Simple, fixed-size, stack-type arena (aka bump allocator).
// TODO Make growable: reserve, but don't commit, a huge block of memory.
// TODO Linux implementation.
// TODO Implement fixed-size ring buffer on top of arena,
//      both with fixed-length and variable-length elements.

typedef struct
{
    byte* data;
    usize len;
    usize pos;
} arena;

typedef struct
{
    arena* a;
    usize pos_saved;
} arena_tmp;

// Create a fixed-size arena. Commit all memory immediately. The parameter `initial_size` (in bytes)
// must be nonzero.
//
// Return: arena on success; stub (all-0) on error.
//
arena arena_create(usize initial_size)
{
    arena a = {0};
    if (initial_size == 0) {
        assertm(false, "Cannot create an empty arena.");
        return a;
    }
    byte* data = NULL;
  #ifdef _WIN32
   // TODO Make it growable: VirtualAlloc(... MEM_RESERVE ...)
    data = (byte*) VirtualAlloc(
            NULL,
            initial_size,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
    bool success = data != NULL;
  #else
    // On some systems, munmap() requires the length to be a multiple of the page size, so
    // we round it up to the nearest multiple.
    usize page_size = sysconf(_SC_PAGESIZE);
    initial_size = page_size * ((initial_size-1) / page_size + 1);
    data = (byte*)mmap(
            NULL,
            initial_size,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0
        );
    bool success = data != MAP_FAILED;
  #endif
    if (success) {
        a.len = initial_size;
        a.pos = 0;
        a.data = data;
    } else {
        // Nothing to do (a is already a stub).
    }
    return a;
}

// Release the arena, i.e., deallocate its memory. If arena was already released, do nothing.
//
// Return: true on success; false on error.
//
bool arena_destroy(arena* a)
{
    if (!a->data) {
        return true;
    }
  #ifdef _WIN32
    bool success = VirtualFree(a->data, 0, MEM_RELEASE);
  #else
    bool success = 0 == munmap(a->data, a->len);
  #endif
    if (success) {
        a->data = 0;
        a->len = 0;
        a->pos = 0;
    }
    return success;
}

// Reset the arena to empty. Do not deallocate/decommit any memory.
void arena_clear(arena* a)
{
    a->pos = 0;
}

// Return a temporary (sub-lifetime) arena, built on top of an existing arena. No new memory is
// allocated.
arena_tmp arena_tmp_begin(arena* a)
{
    arena_tmp tmp = {0};
    tmp.a = a;
    tmp.pos_saved = tmp.a->pos;
    return tmp;
}

// Restore the original arena to its state before arena_tmp_begin() was called.
void arena_tmp_end(arena_tmp tmp)
{
    if (tmp.a) {
        tmp.a->pos = tmp.pos_saved;
    }
}

// Obtain some temporary memory. If other arenas are present in the local scope, and if aliasing is
// possible (i.e., the other arenas might themselves be scratch arenas), then the coller should pass
// them in as conflicts.
//
// Typical usage:
//      arena_tmp scratch = scratch_get(NULL, 0);
// or:  arena_tmp scratch = scratch_get(&&arena, 1);
//      // ... do stuff with the arena *scratch.a ...
//      scratch_release(scratch);
//
// Returns a stub if there are no available scratch arenas.
//
// Source: https://www.rfleury.com/p/untangling-lifetimes-the-arena-allocator)
//
#define MAX_SCRATCH_ARENAS 2
#define SCRATCH_ARENA_SIZE (8 * 1024 * 1024)
arena_tmp scratch_get(arena** conflicts, usize conflict_count)
{
    // TODO Make scratch arenas thread-local.
    // TODO Use growable scratch arenas.
    static arena scratch[MAX_SCRATCH_ARENAS] = {0};
    bool does_conflict[MAX_SCRATCH_ARENAS] = {0};
    for (usize i = 0; i < conflict_count; ++i) {
        for (usize j = 0; j < MAX_SCRATCH_ARENAS; ++j) {
            if (&scratch[j] == conflicts[i]) {
                does_conflict[j] = true;
            }
        }
    }
    for (usize j = 0; j < MAX_SCRATCH_ARENAS; ++j) {
        if (!does_conflict[j]) {
            if (!scratch[j].data) {
                arena new_arena = arena_create(SCRATCH_ARENA_SIZE);
                memcpy(&scratch, &new_arena, sizeof new_arena);
            }
            return arena_tmp_begin(&scratch[j]);
        }
    }
    assertm(false, "No available (non-conflicting) scratch arenas.");
    arena_tmp empty_arena_tmp = {0};
    return empty_arena_tmp;
}
#define scratch_release(tmp) arena_tmp_end(tmp)

// Push uninitialized space onto the arena.
//
// Return: pointer to space on success; null on error.
//
byte* arena_push(arena* a, usize len)
{
    if (a->pos + len > a->len) {
        // Not enough space.
        // TODO Grow the arena: VirtualAlloc(..., MEM_COMMIT,...); update a->len.
        assertm(false, "No space remaining in arena. Growing unimplemented.");
        return 0;
    }
    byte* dst = a->data + a->pos;
    a->pos += len;
    return dst;
}

// Push a copy of the given object onto the arena.
//
// Return: pointer to the copy on success; null on error.
//
byte* arena_push_obj(arena* a, byte* obj, usize len)
{
    byte* dst = arena_push(a, len);
    if (dst) {
        memcpy(dst, obj, len);
    }
    return dst;
}

// Push a block of zeroed memory onto the arena.
//
// Return: pointer to the block on success; null on error.
//
byte* arena_push_zero(arena* a, usize len)
{
    if (a->pos + len > a->len) {
        // Not enough space.
        assertm(false, "No space remaining in arena. Growing unimplemented.");
        return 0;
    }
    byte* dst = a->data + a->pos;
    memset(dst, 0, len);
    a->pos += len;
    return dst;
}

// Helpers (syntax sugar).
#define arena_push_array(a, type, count) (type*)arena_push((a), sizeof(type)*(count))
#define arena_push_array_zero(a, type, count) (type*)arena_push_zero((a), sizeof(type)*(count))
#define arena_push_struct(a, type) arena_push_array((a), (type), 1)
#define arena_push_struct_zero(a, type) arena_push_array_zero((a), (type), 1)

// Pop the requested number of bytes off the arena. If obj != NULL, copy the data into obj.
//
// Return: true on success; false on error.
//
bool arena_pop(arena* a, usize len, byte* obj)
{
    if (len > a->pos) {
        return false;
    }
    a->pos -= len;
    if (obj) {
        memcpy(obj, a->data + a->pos, len);
    }
    return true;
}

// Dynamic array (on top of arena). Elements may be arbitrary structs.
//
// Usage example:
//   typedef_darray(mystruct);  // Already defined for some numeric types such as i32.
//   darray_mystruct vec = darray_mystruct_new(&arena, 100);
//
// Inspired by: https://nullprogram.com/blog/2023/10/05/
#define typedef_darray(T)                                               \
                                                                        \
/* A dynamic array. */                                                  \
typedef struct                                                          \
{                                                                       \
        T* data;                                                        \
    usize len;  /* Number of elements. */                               \
    usize cap;  /* Number of elements. */                               \
} darray_##T;                                                           \
                                                                        \
/* Allocate a new dynamic array within the given arena, */              \
/* for elements of size `element_size` bytes, initial   */              \
/* length 0, and initial capacity `initial_cap`.        */              \
darray_##T darray_##T##_new(arena* a, usize initial_cap)                \
{                                                                       \
    darray_##T darr;                                                    \
    darr.data = arena_push_array_zero(a, T, initial_cap);               \
    assertm(darr.data, "Failed to allocate dynamic array.");            \
    darr.len = 0;                                                       \
    darr.cap = initial_cap;                                             \
    return darr;                                                        \
}                                                                       \
                                                                        \
/* Return the address of the new (unitialized!) element. */             \
T* darray_##T##_push(arena* a, darray_##T* darr)                        \
{                                                                       \
    if (darr->len == darr->cap) {                                       \
        /* Out of space: must grow. */                                  \
        usize new_cap = MAX(1, 2 * darr->cap);                          \
        darray_##T darrnew = darray_##T##_new(a, new_cap);              \
        memcpy(darrnew.data, darr->data, darr->len * sizeof(T));        \
        darrnew.len = darr->len;                                        \
        *darr = darrnew;                                                \
    }                                                                   \
    ++darr->len;                                                        \
    assertm(darr->cap >= darr->len, "Array overflow.");                 \
    return darr->data + darr->len - 1;                                  \
}                                                                       \
                                                                        \
usize darray_##T##_size(darray_##T darr) { return darr.len; }           \
                                                                        \
bool darray_##T##_empty(darray_##T darr) { return darr.len == 0; }      \
                                                                        \
void darray_##T##_clear(darray_##T* darr) { darr->len = 0; }            \
                                                                        \
T darray_##T##_pop(darray_##T* darr)                                    \
{                                                                       \
    assertm(darr->len > 0, "Dynamic array: Cannot pop from empty.");    \
    return darr->data[darr->len--];                                     \
}                                                                       \
                                                                        \
T darray_##T##_remove(darray_##T* darr, usize idx)                      \
{                                                                       \
    assertm(idx < darr->len, "Dynamic array index out of bounds.");     \
    T item = *(darr->data + darr->len);                                 \
    --darr->len;                                                        \
    for (usize i = idx; i < darr->len; ++i) {                           \
        darr->data[i] = darr->data[i+1];                                \
    }                                                                   \
    return item;                                                        \
}                                                                       \
                                                                        \
T* darray_##T##_insert(arena* a, darray_##T* darr, usize idx, T item)   \
{                                                                       \
    assertm(idx <= darr->len, "Dynamic array index out of bounds.");    \
    darray_##T##_push(a, darr);  /* Make space for new item. */         \
    for (usize i = darr->len-1; i > idx; --i) {                         \
        darr->data[i] = darr->data[i-1];                                \
    }                                                                   \
    darr->data[idx] = item;                                             \
    return &darr->data[idx];                                            \
}                                                                       \

// Define dynamic arrays for simple types (such as i32).
#define X(T) \
typedef_darray(T);
FOR_BASIC_TYPES
#undef X
