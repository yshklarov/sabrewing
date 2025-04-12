#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOGGER_CAP 64*1024
#define LOGGER_MEMORY 8*1024*1024
#define LOGGER_MAX_ENTRYSIZE 8192

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

typedef enum
{
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_CRITICAL,
    LOG_LEVEL_COUNT,
} log_level;

typedef struct
{
    u32 len;
    #ifdef _MSC_VER
    // MSVC Complains if we compile in C++ mode.
    #pragma warning( push )
    #pragma warning( disable : 4200 )
    #endif
    u8 data[];  // Not null-terminated.
    #ifdef _MSC_VER
    #pragma warning( pop )
    #endif
} logger_str;

typedef struct
{
    timedate timestamp;
    log_level level;
    // Contiguous, with variable-length entries, for fast search.
    logger_str* content;
} logger_entry;

// TODO Make the logger into a ring buffer, overwriting old messages. This requires:
//      - Implement a fixed-entry-length ring buffer (on top of arena), for store_entries.
//      - Implement a variable-entry-length ring buffer (on top of arena), for store_strs.
//      - Build a way to iterate over the contents of bothe the fixed- and variable-length buffers,
//        e.g., for full-text search.
typedef struct
{
    u32 cap;
    u32 len;
    arena store_entries;
    arena store_strs;
    logger_entry* entries;  // Contiguous array.
} logger;

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
// E.g., [2000-01-01 08:12:34] (needs buf_len >= 21+1 = 22; otherwise, will truncate).
//
// Return the number of characters printed, not including the null terminator.
//
u32 print_timedate(u8* buf, u32 buf_len, timedate td)
{
    #define TIMEDATE_FMT_LEN 22
    char const* format_str = "[%04u-%02u-%02u %02u:%02u:%02u]";
    return (u32)snprintf((char*)buf, buf_len,
             format_str,
             td.year, td.month, td.day, td.hour, td.minute, td.second);
}

void logger_destroy(logger* l);
logger logger_create()
{
    // Create two arenas
    logger l = {0};
    l.cap = LOGGER_CAP;
    l.len = 0;
    l.store_entries = arena_create(LOGGER_CAP * sizeof(logger_entry));
    l.store_strs = arena_create(LOGGER_MEMORY);
    l.entries = (logger_entry*)l.store_entries.data;
    if (!l.store_entries.data || !l.store_strs.data) {
        assertm(false, "Failed to allocate memory for new logger.");
        logger_destroy(&l);
    }
    return l;
}

// Return the message as a null-terminated UTF-8 string, in a statically-allocated object (shared
// across calls).
// If the index is invalid, return a null pointer.
u8 const* logger_get_message(logger l, u32 index)
{
    if (index >= l.len) {
        return NULL;
    }
    #define MESSAGE_MAX_LEN (LOGGER_MAX_ENTRYSIZE + 1)
    static u8 message[MESSAGE_MAX_LEN] = {0};
    memset(message, 0, sizeof(message));
    u32 offset = 0;
    memcpy(message + offset,
           l.entries[index].content->data,
           l.entries[index].content->len);
    offset += l.entries[index].content->len;
    message[offset++] = '\0';
    return message;
    #undef MESSAGE_MAX_LEN
}

// Return the message, with a timestamp, as a null-terminated UTF-8 string, in a
// statically-allocated object (shared across calls).
// If the index is invalid, return a null pointer.
// TODO Also allow printing the log level. In fact, switch to this simpler API:
//      logger_get_message(logger l, u32 index, bool timestamp, bool log_level);
u8 const* logger_get_message_with_timestamp(logger l, u32 index)
{
    if (index >= l.len) {
        return NULL;
    }
    #define MESSAGE_MAX_LEN (TIMEDATE_FMT_LEN + LOGGER_MAX_ENTRYSIZE + 1)
    static u8 message[MESSAGE_MAX_LEN] = {0};
    memset(message, 0, sizeof(message));
    u32 offset = print_timedate(message, MESSAGE_MAX_LEN, l.entries[index].timestamp);
    message[offset++] = ' ';
    memcpy(message + offset,
           l.entries[index].content->data,
           l.entries[index].content->len);
    offset += l.entries[index].content->len;
    message[offset++] = '\0';
    return message;
    #undef MESSAGE_MAX_LEN
}

void logger_destroy(logger* l)
{
    l->cap = 0;
    l->len = 0;
    arena_destroy(&l->store_entries);
    arena_destroy(&l->store_strs);
    l->entries = 0;
}

void logger_clear(logger* l)
{
    l->len = 0;
    arena_clear(&l->store_entries);
    arena_clear(&l->store_strs);
}

// Add a new log entry. Here `message` is a null-terminated UTF-8 string. Messages that are too
// long will be truncated.
//
// Return: True on success; false on failure (e.g., out of memory).
//
bool logger_append(logger* l, log_level level, char* message)
{
    if (l->len == l->cap) {
        return false;
    }
    u32 data_len = (u32)strlen(message);
    data_len = MIN(LOGGER_MAX_ENTRYSIZE, data_len);

    // Allocate memory in our two arenas.
    bool success = true;
    logger_entry* e = (logger_entry*)arena_push(&l->store_entries, sizeof(logger_entry));
    logger_str* s = NULL;
    if (!e) {
        success = false;
    } else {
        s = (logger_str*)arena_push(&l->store_strs, sizeof(logger_str) + data_len);
        if (!s) {
            // Roll back.
            arena_pop(&l->store_entries, sizeof(logger_entry), NULL);
            success = false;
        }
    }

    // Initialize the memory.
    if (success) {
        e->timestamp = get_timedate();
        e->level = level;
        e->content = s;
        s->len = data_len;
        memcpy(s->data, message, data_len);
        ++l->len;
    }

    return success;
}

// Add a new log entry. Like logger_apend(), but with formatting like printf().
bool logger_appendf(logger* l, log_level level, char* fmt, ...)
{
    char buf[LOGGER_MAX_ENTRYSIZE] = {0};
    va_list vlist;
    va_start(vlist, fmt);
    vsnprintf((char*)buf, sizeof(buf), fmt, vlist);
    va_end(vlist);
    return logger_append(l, level, buf);
}
