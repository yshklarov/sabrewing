// TODO Add option to "warm up" the CPU to bring it up to its base (or boost?) clock rate
//      before starting the main workload.
// TODO query_host_info() should get the realtime clock rate (like in CPU-Z).
// TODO Add: RDTSCP
// TODO Add: timing method(s) for Linux: clock_gettime with CLOCK_MONTONIC_RAW.
// TODO Add: HPET and/or ACPI PM timers
// TODO Add: RDPMC (may require some sort of privilege escalation)
// TODO Add: RDPRU for certain AMD Zen CPUs, because RDTSC/RDTSCP is too coarse-grained on those.
// TODO Look into Intel PCM (Performance Counter Montitor) for detailed diagnostics, such as:
//      L2/L3 cache hits/misses, branches, core frequency, instructions retired, memory bytes
//      read/written, etc.
// TODO Look into Windows PMU (for branch mispredictions, cache misses, etc.): See
//      https://learn.microsoft.com/en-us/windows-hardware/test/wpt/recording-pmu-events
// TODO Add: ARM GIT (Generic Interval Timer)
// TODO Add: ARM Performance Monitors Cycle Counter
// TODO Find a way to detect process pre-empting, so we can restart the test, so we can
//      save time.


/**** Types ****/

typedef enum
{
    TIMING_RDTSC,
    TIMING_QPC,
    TIMING_QTCT,
    TIMING_QPCT,
    TIMING_METHOD_ID_MAX
} timing_method_id;

typedef struct
{
    char const * name_short;
    char const * name_long;
    bool available_linux;
    bool available_win32;
} timing_method;

static timing_method timing_methods[TIMING_METHOD_ID_MAX] =
{
    // Do not re-order or delete methods (to avoid corrupting savefiles).
    { "RDTSC", "X86 Time Stamp Counter (RDTSC)", true, true },
    { "QPC", "Win32 QueryPerformanceCounter (QPC)", false, true },
    { "QTCT", "Win32 QueryThreadCycleTime (QTCT)", false, true },
    { "QPCT", "Win32 QueryProcessCycleTime (QPCT)", false, true },
};

/*
typedef enum
{
    OS_WINDOWS,
    OS_LINUX
} operating_system;
*/

typedef struct
{
    bool initialized;
    //operating_system os;
    char cpu_name[48];
    u32 cpu_num_cores;
    u32 cpu_cache_l1;
    u32 cpu_cache_l2;
    u32 cpu_cache_l3;
    u64 qpc_frequency;
    u64 tsc_frequency;
    bool has_tsc;
    bool has_invariant_tsc;
} host_info;

typedef struct
{
    // Sampler parameters
    // These are i32 out of necessity for the time being (ImGui standard widgets require it).
    // TODO Switch to range_u32 after writing custom ImGui range widget.
    range_i32 ns;
    i32 sample_size;
    u64 seed;
    bool seed_from_time;

    // Other parameters
    i32 sampler_idx;
    i32 target_idx;
    timing_method_id timing;
    bool adjust_for_timer_overhead;
    // TODO Implement a smarter repeat method: Adaptive: Repeat until the three best results are
    //      within <xx>% of one another, then take the mean of these three.
    //repeat_method repeat;
    i32 repetitions;
    bool verify_correctness;
}  profiler_params;

profiler_params profiler_params_default()
{
    profiler_params rtn;

    rtn.ns.lower = 0;
    rtn.ns.stride = 1;
    rtn.ns.upper = 200;
    rtn.sample_size = 10;
    rtn.seed = 0;
    rtn.seed_from_time = false;

    rtn.sampler_idx = 0;
    rtn.target_idx = 0;
    rtn.timing = TIMING_RDTSC;
    rtn.adjust_for_timer_overhead = false;
    rtn.repetitions = 10;
    rtn.verify_correctness = true;

    return rtn;
}

typedef struct
{
    rand_state seed;  // (sizeof u64)*4 = 32 bytes
    f64 n;
    f64 time;  // nanoseconds
    // TODO Add metadata:
    //   - RNG state used (se we can re-create the input)
} profiler_result_unit;

// Summary statistics for a batch of test units.
typedef struct
{
    f64 n;
    f64 time_min;
    f64 time_max;
    f64 time_mean;
    f64 time_median;
} profiler_result_group;


typedef struct
{
    // TODO Add metadata:
    //   - Wall time began (call get_timedate() from util.c)
    //   - Wall time completed
    //   - Total wall time elapsed (yes, store separately, because of possible time changes)

    arena local_arena;  // Memory container; lifetime is until this result is freed.
    u64 id;  // id==0 indicates that this is a stub (uninitialized, or already destroyed).

    profiler_params params;
    u32* input;  // Scratch space for storing input to targets.

    u64 len_units;
    u64 len_groups;
    profiler_result_unit* units;
    profiler_result_group* groups;

    u64 verification_failure_count;

    bool plot_visible;  // TODO Move this out of here, into a GUI state struct.
} profiler_result;
typedef_darray(profiler_result);


/**** Functions ****/

void result_destroy(profiler_result* result);

// Initialize result. On failure, make a stub (result == {0}).
// If this call succeeds, the caller must eventually call result_destroy().
profiler_result result_create(profiler_params params)
{
    profiler_result result = {0};

    u64 len_groups = range_i32_count(params.ns);
    if ((u64)len_groups * (u64)params.sample_size > (u64)I32_MAX) {
        // Too many units.
        goto error_memory;
    }
    u64 len_units = len_groups * params.sample_size;

    // Reserve local memory for the result..
    usize arena_len_required = 0;
    arena_len_required += params.ns.upper * sizeof(params.ns.upper);  // result.input
    arena_len_required += len_units * sizeof(profiler_result_unit);
    arena_len_required += len_groups * sizeof(profiler_result_group);
    result.local_arena = arena_create(arena_len_required);
    if (!result.local_arena.data) {
        // Failed to allocate memory arena.
        goto error_memory;
    }

    static u64 unique_id = 1;
    result.id = unique_id++;
    result.params = params;
    result.input = arena_push_array_zero(&result.local_arena, u32, params.ns.upper);
    result.len_units = len_units;
    result.len_groups = len_groups;
    result.units = arena_push_array_zero(&result.local_arena, profiler_result_unit, len_units);
    result.groups = arena_push_array_zero(&result.local_arena, profiler_result_group, len_groups);

    return result;

error_memory:
    result_destroy(&result);
    return result;
}

void result_destroy(profiler_result* result)
{
    arena_destroy(&result->local_arena);
    static profiler_result empty_result = {0};
    *result = empty_result;
}


// Estimate the frequency of the TSC, in Hz.
//
// This is done by measurement, by calling out to another (monotonic, high-resolution) OS-provided
// wall timer. Note that it's not possible to do this in any other way (except by manually building
// a comprehensive database of CPU models) because the majority of x86 CPUs do not have instructions
// to provide this data.
//
// The first time this function is called, it will return 0.
// If invariant_tsc == true, it will return more accurate successive estimates each time it's called.
// If invariant_tsc == false, it will only use the last two times it was called as an interval
// for measurement; the result will be more accurate, but less precise.
//
// Note: It would also possible to fetch the TSC frequency from the Linux kernel.
//
u64 measure_tsc_frequency(bool invariant_tsc)
{
    // TODO Eliminate these global variables -- they should be passed in as part of a host_info.
    static bool already_called = false;
    static u64 tsc_initial = 0;
    static u64 wall_time_initial = 0;
    if (!already_called) {
        wall_time_initial = get_ostime_100ns();
        _mm_lfence();
        tsc_initial = __rdtsc();
        already_called = true;
        return 0;
    }

    _mm_lfence();
    u64 tsc_now = __rdtsc();
    u64 tsc_elapsed = tsc_now - tsc_initial;

    u64 wall_time_now = get_ostime_100ns();
    u64 wall_time_elapsed = wall_time_now - wall_time_initial;

    if (!invariant_tsc) {
        // We need to keep re-measuring, because the frequency changes.
        tsc_initial = tsc_now;
        wall_time_initial = wall_time_now;
    }

    return 10000000u * tsc_elapsed / MAX(1ll, wall_time_elapsed);
}

#ifdef _WIN32
u64 get_qpc_frequency()
{
    // The QPC frequency is fixed at system boot, and guaranteed not to change.
    static LARGE_INTEGER qpc_frequency = {0};
    if (!qpc_frequency.QuadPart) {
        QueryPerformanceFrequency(&qpc_frequency);
    }
    return qpc_frequency.QuadPart;
}
#endif

// Return the value of the given timer.
u64 get_timer_value(timing_method_id tmid)
{
    // This is branchy, but not overly so. If the user cares about the extra dozen instructions due
    // to calling this function (as opposed to calling, say, __rdtsc() directly), they can adjust by
    // subtracting get_timer_overhead().
    switch(tmid) {
    case TIMING_RDTSC: {
        _mm_lfence();
        return __rdtsc();
    } break;
    case TIMING_QPC: {
        #ifdef _WIN32
        LARGE_INTEGER qpc_now;
        QueryPerformanceCounter(&qpc_now);
        return qpc_now.QuadPart;
        #else
        assertm(false, "The requested timing method is unavailable on this platform.");
        return 0;
        #endif
    } break;
    case TIMING_QTCT: {
        #ifdef _WIN32
        u64 time_now;
        HANDLE current_thread = GetCurrentThread();
        QueryThreadCycleTime(current_thread, &time_now);
        return time_now;
        #else
        assertm(false, "The requested timing method is unavailable on this platform.");
        return 0;
        #endif
    } break;
    case TIMING_QPCT: {
        #ifdef _WIN32
        u64 time_now;
        HANDLE current_process = GetCurrentProcess();
        QueryProcessCycleTime(current_process, &time_now);
        return time_now;
        #else
        assertm(false, "The requested timing method is unavailable on this platform.");
        return 0;
        #endif
    } break;
    default: {
        assertm(false, "The requested timing method is unimplemented.");
        return 0;
    } break;
    }
}

// Return the frequency of the given timer in Hz.
u64 get_timer_frequency(timing_method_id tmid, host_info* host)
{
    switch(tmid) {
    case TIMING_RDTSC: {
        return measure_tsc_frequency(host->has_invariant_tsc);
    } break;
    case TIMING_QPC: {
        #ifdef _WIN32
        return get_qpc_frequency();
        #else
        assertm(false, "The requested timing method is unimplemented.");
        return 0;
        #endif
    } break;
    case TIMING_QTCT:
    case TIMING_QPCT: {
        #ifdef _WIN32
        // MSDN doesn't recommend this. But we have to do it somehow, and this seems to work.
        return measure_tsc_frequency(host->has_invariant_tsc);
        #else
        assertm(false, "The requested timing method is unimplemented.");
        return 0;
        #endif
    } break;
    default: {
        assertm(false, "The requested timing method is unimplemented.");
        return 0;
    } break;
    }
}

// Get the overhead resulting from using the timer: time between successive calls.
// This function tests a large number of repetitions to get a good measurement.
u64 get_timer_overhead(timing_method_id tmid, u32 timeout_milliseconds)
{
    // Our process might be pre-empted, so we need to do this many times to be very sure that we
    // don't over-estimate the overhead.

    // This is stupid, but it works -- for most timing methods. Except QueryProcessCycleTime is
    // finicky and unreliable; trying to measure it gives extremely unpredictable results. Still, we
    // allow it, in case the user really wants it.

    // Note also: Some timing methods (QPCT!) are simply inconsistent in how long they take; still,
    // we look for the *minimum* time, because it would very bad to over-estimate.

    u64 timeout_us = timeout_milliseconds * 1000;
    u64 start_time = get_ostime_us();

    // "Good values" are those that are very close to the minimum value found so far.
    u64 min_overhead = U64_MAX;
    u32 good_values_found = 0;
    u32 good_values_needed = 30;
    bool done = false;
    while (!done) {
        for (u32 i = 0; i < 5; ++i) {
            u64 one = get_timer_value(tmid);
            u64 two = get_timer_value(tmid);
            u64 overhead = two - one;
            if (overhead < min_overhead) {
                min_overhead = overhead;
                good_values_found = 1;
            } else {
                if ((overhead == min_overhead) ||
                    (min_overhead / (overhead - min_overhead) > 30)) {
                    ++good_values_found;
                    if (good_values_found == good_values_needed) {
                        done = true;
                        break;
                    }
                }
            }
        }
        if (get_ostime_us() - start_time >= timeout_us) {
            break;
        }
    }
    return min_overhead;
}



// Probe the host for general information about the processor, etc.
// Note: More detailed CPU information is available through Sysinternals Coreinfo and CPU-Z.
void query_host_info(host_info* host)
{
    if (!host->initialized) {
        get_cpu_brand(host->cpu_name);
        host->cpu_num_cores = get_cpu_num_logical_processors();
        get_cpu_tsc_features(
                &host->has_tsc,
                &host->has_invariant_tsc);
        get_cpu_data_cache_sizes(
                &host->cpu_cache_l1,
                &host->cpu_cache_l2,
                &host->cpu_cache_l3);
        #ifdef _WIN32
        // QPC frequency is fixed at system boot.
        host->qpc_frequency = get_qpc_frequency();
        #else
        host->qpc_frequency = 0;
        #endif
        host-> initialized = true;
    }

    // Measure continually, because (on some systems) it may change, and in any case
    // we can get a more precise estimate by measuring over longer time periods.
    host->tsc_frequency = measure_tsc_frequency(host->has_invariant_tsc);
}


profiler_result profiler_execute(logger* l, profiler_params params, host_info* host)
{
    logger_append(l, LOG_LEVEL_INFO, "Starting profiler run.");

    // Save metadata and results for this profiler run.
    profiler_result result = result_create(params);
    if (!result.id) {
        logger_append(l, LOG_LEVEL_ERROR, "Failed to allocate new profiler result.");
        return result;
    }

    // TODO Run profiler in separate function and separate thread; setting GUI to low priority
    //      (or to low FPS cap). Or even in a separate process!

    u64 sample_size = params.sample_size;  // For brevity.
    fn_sampler_sort sampler = samplers[params.sampler_idx].fn;
    fn_target_sort target = targets[params.target_idx].fn;

    // It would be nice to re-measure the overhead for every call of the target, just in case
    // the overhead is changing (with CPU scaling, system load, etc.); however, if we do that,
    // sometimes measuring the overhead gives an overly-high value (due to thread/process
    // pre-empting or other OS scheduler shenanigans). So, for now, we only measure once.
    // TODO When there is an Invariant TSC, we could be more aggressive in finding the _minimum_
    //      overhead (storing it across profiler runs), because it sometimes varies by 1-2 ns.
    u64 timer_overhead =
        params.adjust_for_timer_overhead
        ? get_timer_overhead(params.timing, 10)
        : 0;

    for (i32 rep = 0; rep < params.repetitions; ++rep) {
        // Each repetition must use the same sample, so we re-seed here.
        rand_state rand_state_local;
        rand_init_from_seed(&rand_state_local, params.seed);

        u64 n_idx = 0;
        // TODO Prettier range loop.
        for (i32 n = params.ns.lower;
             n <= params.ns.upper;
             n += params.ns.stride) {
            for (u64 i = 0; i < (u64)sample_size; ++i) {
                arena_tmp scratch = scratch_get(NULL, 0);
                result.units[n_idx * sample_size + i].n = (f64)n;

                // Generate input data for this test unit. We do this inside the loop, just before
                // measuring, to encourage the input data to already be in CPU cache when the
                // critical code begins.
                sampler(result.input, n, &rand_state_local, scratch.a);

                // Store information about the input, in order to verify correctness later.
                u64 checksum_before = 0;
                if (params.verify_correctness) {
                    checksum_before = verify_checksum(result.input, n);
                }

                // Measure the execution time of our target function.
                u64 timer_initial = get_timer_value(params.timing);
                target(result.input, n, &rand_state_local, scratch.a);
                u64 timer_final = get_timer_value(params.timing);
                u64 timer_delta = timer_final - timer_initial;

                // Adjust for the time it takes to call the timing subroutines themselves.
                if (params.adjust_for_timer_overhead) {
                    if (timer_overhead < timer_delta) {
                        timer_delta -= timer_overhead;
                    } else {
                        timer_delta = 0;
                    }
                }

                // Convert to wall time. Re-query the timer frequency *every time*, just in case
                // it's changing (e.g., non-invariant TSC).
                u64 timer_frequency = get_timer_frequency(params.timing, host);
                f64 timer_delta_ns = 1e9 * (f64)timer_delta / (f64)timer_frequency;

                // Save to result data.
                if (rep == 0) {
                    result.units[n_idx * sample_size + i].time = timer_delta_ns;
                } else {
                    f64 best_so_far = result.units[n_idx * sample_size + i].time;
                    result.units[n_idx * sample_size + i].time = MIN(timer_delta_ns, best_so_far);
                }

                // Verify correctness of output.
                // TODO Have an option to do "simple" or "full" correctness, where the full
                //      check will use a known-reliable sort and compare to the output.
                if (rep == 0 && params.verify_correctness) {
                    // TODO Also mark these units "verification_failed = true", and color them red.
                    if (!verify_ordered(result.input, n)) {
                        ++result.verification_failure_count;
                    } else {
                        u64 checksum_after = verify_checksum(result.input, n);
                        if (checksum_before != checksum_after) {
                            ++result.verification_failure_count;
                        }
                    }
                }
                scratch_release(scratch);
            }
            ++n_idx;
        }
    } // for (rep ...)

    if (params.verify_correctness) {
        if (result.verification_failure_count == 0) {
            logger_appendf(l, LOG_LEVEL_INFO,
                           "Verification success: Output correct for all %llu units.",
                           result.len_units);
        } else {
            logger_appendf(l, LOG_LEVEL_INFO,
                           "Verification failure: Incorrect output for %llu/%llu units.",
                           result.verification_failure_count, result.len_units);
        }
    }


    // Gather result for plotting.
    arena_tmp scratch = scratch_get(0, 0);
    f64* times = arena_push_array_zero(scratch.a, f64, sample_size);
    // TODO Prettier range loop.
    u64 n_idx = 0;
    for (u64 n = params.ns.lower;
         n <= (u64)params.ns.upper;
         n += params.ns.stride) {
        result.groups[n_idx].n = (f64)n;
        result.groups[n_idx].time_mean = 0;
        // TODO Don't copy and sort; instead, implement quickselect to find quantiles and median.
        for (u64 i = 0; i < (u64)sample_size; ++i) {
            times[i] = result.units[n_idx * sample_size + i].time;
            result.groups[n_idx].time_mean += times[i];
        }
        result.groups[n_idx].time_mean /= sample_size;
        util_sort(times, (u32)sample_size);
        result.groups[n_idx].time_min = times[0];
        result.groups[n_idx].time_max = times[sample_size - 1];
        if (sample_size & 1) {
            result.groups[n_idx].time_median = times[(sample_size - 1)/2];
        } else {
            result.groups[n_idx].time_median =
                (times[(sample_size - 1)/2] +
                 times[(sample_size - 1)/2 + 1]) / 2;
        }
        ++n_idx;
    }
    scratch_release(scratch);

    result.plot_visible = true;

    logger_appendf(l, LOG_LEVEL_INFO, "Completed profiler run (ID %d).", result.id);
    return result;
}
