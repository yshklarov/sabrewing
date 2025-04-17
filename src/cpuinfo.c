// Functions for obtaining information about the processor.
// Currently, these work only for Intel and AMD CPUs.

#ifdef _WIN32
#include <intrin.h>
#define REG32 i32
#define CPUID(r, leaf) (__cpuid(r, leaf))
#else
#include <cpuid.h>
#define REG32 u32
#define CPUID(r, leaf) (__get_cpuid(leaf, (r)+0, (r)+1, (r)+2, (r)+3))
#endif

#include <string.h>

// For directions on obtaining more information from CPUID, see:
//     https://learn.microsoft.com/en-us/cpp/intrinsics/cpuid-cpuidex?view=msvc-170

// Write the CPU name string to `brand`, which must be a char array of length 48.
void get_cpu_brand(char* brand)
{
    REG32 regs[4] = {0};  // EAX, EBX, ECX, EDX
    CPUID(regs, 0x80000000);
    if (regs[0] < 0x80000004) {
        #ifdef _WIN32
        #pragma warning(push)
        #pragma warning(disable : 4996)
        #endif
        strncpy(brand, "Unknown", 48);
        #ifdef _WIN32
        #pragma warning(pop)
        #endif
    } else {
        for (i32 i = 0; i < 3; i++) {
            CPUID(regs, 0x80000002 + i);
            memcpy(&brand[i * 16], regs, 16);
        }
    }
    brand[47] = '\0';  // Just in case.
}

void get_cpu_tsc_features(bool* has_tsc, bool* has_invariant_tsc)
{
    *has_tsc = false;
    *has_invariant_tsc = false;

    // Use CPUID to retrieve support for features.
    // Reference: https://blog.winny.tech/posts/cpuid/

    // Check support for RDTSC.
    // (leaf 1, EDX bit 4)
    REG32 regs[4] = {0};  // EAX, EBX, ECX, EDX
    CPUID(regs, 1);
    *has_tsc = regs[3] & (1 << 4);

    /*
    // Check support for RDTSCP.
    // (leaf 0x80000001, EDX bit 27)
    // TODO test this.
    CPUID(regs, 0x80000001);
    *has_tsc = regs[3] & (1 << 27);
    */

    // Check for invariant TSC.
    // (leaf 0x80000001, EDX bit 8)
    CPUID(regs, 0x80000000);
    bool has_extended_features = (u32)regs[0] >= 0x80000007;
    if (has_extended_features) {
        CPUID(regs, 0x80000007);
        *has_invariant_tsc = regs[3] & (1 << 8);
    }
}

// Get cache size totals in bytes, for each of L1, L2, and L3 data and unified caches.
// Return the cache available to a single core, _not_ the total across all cores.
void get_cpu_data_cache_sizes(u32* l1, u32* l2, u32* l3)
{
    REG32 regs[4] = {0};  // EAX, EBX, ECX, EDX
    *l1 = *l2 = *l3 = 0;

    char vendor[13] = {0};
    CPUID(regs, 0);
    memcpy(vendor + 0, &regs[1], 4);  // EBX
    memcpy(vendor + 4, &regs[3], 4);  // EDX
    memcpy(vendor + 8, &regs[2], 4);  // ECX
    vendor[12] = '\0';

    if (strcmp(vendor, "AuthenticAMD") == 0) {
         CPUID(regs, 0x80000005);
         *l1 = ((regs[2] >> 24) & 0xFF) * 1024; // L1D is in KiB.
         CPUID(regs, 0x80000006);
         *l2 = ((regs[2] >> 16) & 0xFFFF) * 1024;  // L2 is in KiB.
         *l3 = ((regs[3] >> 18) & 0x3FFF) * 512 * 1024;  // L3 is in 512 KiB blocks.
         // Fallback for older L3 detection.
         /* // (Untested)
         if (*l3 == 0) {
             CPUID(regs, 0x80000001);
             // Check ECX bit 9 for L3 presence.
             if (regs[3] & (1 << 9)) {
                 CPUID(regs, 0x80000006);
                 *l3 = ((regs[3] >> 12) & 0x3FFF) * 512 * 1024;
             }
         }
         */
    } else {  // Intel
        CPUID(regs, 0);
        i32 leaf = 4;
        if ((i32)regs[0] < leaf) {
            // No CPUID leaf 4 support.
            return;
        }
        for (i32 subleaf = 0; subleaf < 32; subleaf++) {
            __cpuidex((i32*)regs, leaf, subleaf);
            const i32 cache_type = (i32)regs[0] & 0x1F;
            if (cache_type == 0) {
                // No more caches.
                break;
            }
            if (cache_type != 1 && cache_type != 3) {
                // This is not a data or unified cache.
                continue;
            }
            const i32 cache_level = (regs[0] >> 5) & 0x7;
            if (cache_level < 1 || cache_level > 3) {
                continue;
            }

            const u32 ways = ((regs[1] >> 22) & 0x3FF) + 1;
            const u32 partitions = ((regs[1] >> 12) & 0x3FF) + 1;
            const u32 line_size = (regs[1] & 0xFFF) + 1;
            const u32 sets = regs[2] + 1;
            const u32 cache_size = ways * partitions * line_size * sets;

            switch (cache_level) {
            case 1: { *l1 += cache_size; } break;
            case 2: { *l2 += cache_size; } break;
            case 3: { *l3 += cache_size; } break;
            default: break;
            }
        }
    }
}

u32 get_cpu_num_logical_processors()
{
    // We don't use CPUID here, because its interface is a mess, and different across CPUs.
    // It's much more reliable to ask the OS.
    // Thank you, <https://stackoverflow.com/a/150971/1989005>.
#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    u32 num_cores = (u32)sysinfo.dwNumberOfProcessors;
#else
    u32 num_cores = (u32)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return num_cores;
}
