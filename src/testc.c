// This file exists to test that all C files are actually C. This is needed because:
//
// - I want to write C, and not C++.
// - I want to use Dear ImGui, which is in C++, without having to go through a layer of bindings.
// - To keep the build process very simple, everything is compiled with a C++ compiler as a single
//   translation unit.
// - So the GUI and main() are in a C++ file, which #includes the C files directly.
//
// Periodically run the following commands to test conformance to C99:
//
// Win32:  cl /nologo /W4 /c /FoNUL testc.c
// Linux:  gcc -Wall -Wextra -std=c99 -fsyntax-only testc.c
// Linux:  clang -Wall -Wextra -std=c99 -fsyntax-only testc.c
//
// For additional linting:
//
// Linux: clang-tidy -checks='*' testc.c -header-filter=.* -- -x c


#include "util.c"

#include "cpuinfo.c"
#include "logger.c"
#include "sort.c"

// TODO Make this into a unit test suite.
// TODO (Perhaps) test for out-of-memory errors: https://news.ycombinator.com/item?id=37672784
// TODO Set up convenient testing with valgrind and/or LLVM sanitizers
//      (See: https://nullprogram.com/blog/2023/04/29/)

void test_logger()
{
    logger l = logger_create();
    printf("Logger info: length %d, capacity %d.\n", l.len, l.cap);
    printf("Trying to print a message from empty logger... ");
    if (logger_get_message_with_timestamp(l, 0)) {
        printf("Error: Received non-null!");
    } else {
        printf("Successfully failed.\n");
    }

    puts("");
    puts("Appending to logger...");
    logger_append(&l, LOG_LEVEL_INFO, "1. Hello!");
    logger_append(&l, LOG_LEVEL_INFO, "2. Goodbye!");
    printf("Logger info: length %d, capacity %d.\n", l.len, l.cap);
    printf("Contents:\n");
    for (u32 i = 0; i < l.len; ++i) {
        puts((char*)logger_get_message_with_timestamp(l, i));
    }
    printf("Clearing logger...\n");
    logger_clear(&l);
    printf("Logger info: length %d, capacity %d.\n", l.len, l.cap);

    puts("");
    printf("Overflowing logger...\n");
    u32 num_posted = 0;
    u32 num_lost = 0;
    for (u64 i = 0; i < LOGGER_CAP + 12345; ++i ) {
        if (logger_append(&l, LOG_LEVEL_INFO, "test overflow")) {
            ++num_posted;
        } else {
            ++num_lost;
        }
    }
    printf("Posted: %u. Lost: %u.\n", num_posted, num_lost);
    printf("Logger info: length %d, capacity %d.\n", l.len, l.cap);
    printf("Clearing logger...\n");
    logger_clear(&l);
    printf("Logger info: length %d, capacity %d.\n", l.len, l.cap);

    puts("");
    printf("Testing formatted logging.\n");
    for (i32 i = 1; i <= 20; ++i) {
        logger_appendf(&l, LOG_LEVEL_INFO, "%02d. %s", i, "ASDF");
    }
    printf("Logger info: length %d, capacity %d.\n", l.len, l.cap);
    printf("Contents:\n");
    for (u32 i = 0; i < l.len; ++i) {
        puts((char*)logger_get_message_with_timestamp(l, i));
    }

    logger_destroy(&l);
}

int main()
{
    test_logger();
    return 0;
}
