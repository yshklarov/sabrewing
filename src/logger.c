#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOGGER_CAP 64*1024
#define LOGGER_MEMORY 8*1024*1024
#define LOGGER_MAX_ENTRYSIZE 8192

typedef enum
{
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_CRITICAL,
    LOG_LEVEL_COUNT,
} LogLevel;

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
} LoggerStr;

typedef struct
{
    Timedate timestamp;
    LogLevel level;
    // Contiguous, with variable-length entries, for fast search.
    LoggerStr* content;
} LoggerEntry;

// TODO Make the logger into a ring buffer, overwriting old messages. This requires:
//      - Implement a fixed-entry-length ring buffer (on top of arena), for store_entries.
//      - Implement a variable-entry-length ring buffer (on top of arena), for store_strs.
//      - Build a way to iterate over the contents of bothe the fixed- and variable-length buffers,
//        e.g., for full-text search.
typedef struct
{
    u32 cap;
    u32 len;
    Arena store_entries;
    Arena store_strs;
    LoggerEntry* entries;  // Contiguous array.
} Logger;

void logger_destroy(Logger* l);
Logger logger_create()
{
    // Create two arenas
    Logger l = {0};
    l.cap = LOGGER_CAP;
    l.len = 0;
    l.store_entries = arena_create(LOGGER_CAP * sizeof(LoggerEntry));
    l.store_strs = arena_create(LOGGER_MEMORY);
    l.entries = (LoggerEntry*)l.store_entries.data;
    if (!l.store_entries.data || !l.store_strs.data) {
        assertm(false, "Failed to allocate memory for new logger.");
        logger_destroy(&l);
    }
    return l;
}

// Return the message as a null-terminated UTF-8 string, in a statically-allocated object (shared
// across calls).
// If the index is invalid, return a null pointer.
u8 const* logger_get_message(Logger l, u32 index)
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
//      logger_get_message(Logger l, u32 index, bool timestamp, bool log_level);
u8 const* logger_get_message_with_timestamp(Logger l, u32 index)
{
    if (index >= l.len) {
        return NULL;
    }
    #define MESSAGE_MAX_LEN (TIMEDATE_FMT_LEN + 2 + LOGGER_MAX_ENTRYSIZE + 1)
    static u8 message[MESSAGE_MAX_LEN] = {0};
    memset(message, 0, sizeof(message));
    u32 offset = print_timedate(message, MESSAGE_MAX_LEN, l.entries[index].timestamp, true);
    message[offset++] = ' ';
    memcpy(message + offset,
           l.entries[index].content->data,
           l.entries[index].content->len);
    offset += l.entries[index].content->len;
    message[offset++] = '\0';
    return message;
    #undef MESSAGE_MAX_LEN
}

void logger_destroy(Logger* l)
{
    l->cap = 0;
    l->len = 0;
    arena_destroy(&l->store_entries);
    arena_destroy(&l->store_strs);
    l->entries = 0;
}

void logger_clear(Logger* l)
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
bool logger_append(Logger* l, LogLevel level, char const* message)
{
    if (l->len == l->cap) {
        return false;
    }
    u32 data_len = (u32)strlen(message);
    data_len = MIN(LOGGER_MAX_ENTRYSIZE, data_len);

    // Allocate memory in our two arenas.
    bool success = true;
    LoggerEntry* e = (LoggerEntry*)arena_push(&l->store_entries, sizeof(LoggerEntry));
    LoggerStr* s = NULL;
    if (!e) {
        success = false;
    } else {
        s = (LoggerStr*)arena_push(&l->store_strs, sizeof(LoggerStr) + data_len);
        if (!s) {
            // Roll back.
            arena_pop(&l->store_entries, sizeof(LoggerEntry), NULL);
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
bool logger_appendf(Logger* l, LogLevel level, char const* fmt, ...)
{
    char buf[LOGGER_MAX_ENTRYSIZE] = {0};
    va_list vlist;
    va_start(vlist, fmt);
    vsnprintf((char*)buf, sizeof(buf), fmt, vlist);
    va_end(vlist);
    return logger_append(l, level, buf);
}
