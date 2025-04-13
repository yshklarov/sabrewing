/**** Types ****/

// TODO Add timing method: GetTickCount64() (very coarse: 10-16 ms).
// TODO Add timing method(s) for Linux: UNIX CLOCK_MONTONIC_RAW; ..?

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

typedef struct
{
    bool initialized;
    char cpu_name[48];
    u32 cpu_num_cores;
    u32 cpu_cache_l1;
    u32 cpu_cache_l2;
    u32 cpu_cache_l3;
    u64 qpc_frequency;
    f32 tsc_period;
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
    // TODO Implement a smarter repeat method: Adaptive: Repeat until the three best results are
    //      within 1% of one another, then take the mean of these three.
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
    //rtn.timing = TIMING_QPC;
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
    //   - Various CPU statistics (cache misses, branches, etc.)
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
    //   - Wall time began
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


// Estimate the period of the TSC, in seconds. E.g., return 0.5e-9 if the TSC runs at 2.0 GHz.
//
// The first time this function is called, it will return 0.0.
// If invariant_tsc == true, it will return more accurate successive estimates each time it's called.
// If invariant_tsc == false, it will only use the last two times it was called as an interval
// for measurement.
//
// TODO Debug: Why does this seem to return too fine a period on linux/wine?
//
f32 measure_tsc_period(bool invariant_tsc)
{
    // TODO Eliminate these global variables -- they should be passed in as part of a host_info.
    static bool already_called = false;
    static u64 tsc_initial = 0;
    static u64 wall_time_initial = 0;
    if (!already_called) {
        wall_time_initial = get_time_100ns();
        _mm_lfence();
        tsc_initial = __rdtsc();
        already_called = true;
        return 0.0f;
    }

    // TODO Fences (see https://stackoverflow.com/a/12634857/1989005)
    _mm_lfence();
    u64 tsc_now = __rdtsc();
    u64 tsc_elapsed = tsc_now - tsc_initial;

    u64 wall_time_now = get_time_100ns();
    u64 wall_time_elapsed = wall_time_now - wall_time_initial;

    if (!invariant_tsc) {
        // We need to keep re-measuring, because the frequency changes.
        tsc_initial = tsc_now;
        wall_time_initial = wall_time_now;
    }

    return 0.0000001f * ((f32)wall_time_elapsed / MAX(1ll, tsc_elapsed));
}

#ifdef _WIN32
u64 measure_qpc_frequency()
{
    LARGE_INTEGER qpc_frequency = {0};
    QueryPerformanceFrequency(&qpc_frequency);
    return qpc_frequency.QuadPart;
}
#endif

// Probe the host for general information about the processor, etc.
// Note: More detailed CPU information is available through Sysinternals Coreinfo and CPU-Z.
// TODO Get realtime clock rate (like in CPU-Z).
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
        host->qpc_frequency = measure_qpc_frequency();
     #endif
        host-> initialized = true;
    }

    // Measure continually, because (on some systems) it may change, and in any case
    // we can get a more precise estimate by measuring over longer time periods.
    host->tsc_period = measure_tsc_period(host->has_invariant_tsc);
}


profiler_result profiler_execute(logger* l, profiler_params params, host_info const* host)
{
    logger_append(l, LOG_LEVEL_INFO, "Starting profiler run.");

    // TODO Save Hardware Performance (PMU): branch mispredictions, cache misses, etc.: See
    //      https://learn.microsoft.com/en-us/windows-hardware/test/wpt/recording-pmu-events
    //      And/or Intel getSystemCounterState()?
    // TODO Find a way to detect process pre-empting, so we can restart the test, so we can
    //      save time.

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

    u64 tsc_initial = 0;
    u64 tsc_final = 0;
    u64 tsc_delta = 0;  // (Invariant) TSC units elapsed.
    f32 tsc_period_ns = host->tsc_period * 1000000000;

    #ifdef _WIN32
    LARGE_INTEGER qpc_initial = {0};
    LARGE_INTEGER qpc_final = {0};
    LARGE_INTEGER qpc_delta = {0};
    u64 qpc_delta_ns = 0;  // Unit: Nanoseconds.

    HANDLE current_thread = GetCurrentThread();
    HANDLE current_process = GetCurrentProcess();
    #endif

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

                u64 checksum_before = 0;
                if (params.verify_correctness) {
                    checksum_before = verify_checksum(result.input, n);
                }

                // Measure the execution time of our target function.

                // TODO Measure (once, at the beginning of test run), and subtract, the mean time
                //      to run the timing instructions themselves, i.e., the difference in the
                //      counter between two successive runs.
                switch(params.timing) {
                case TIMING_RDTSC: {
                    _mm_lfence();
                    tsc_initial = __rdtsc();
                    target(result.input, n, &rand_state_local, scratch.a);
                    _mm_lfence();
                    tsc_final = __rdtsc();
                    tsc_delta = tsc_final - tsc_initial;
                } break;
                case TIMING_QPC: {
                    #ifdef _WIN32
                    QueryPerformanceCounter(&qpc_initial);
                    target(result.input, n, &rand_state_local, scratch.a);
                    QueryPerformanceCounter(&qpc_final);
                    qpc_delta.QuadPart = qpc_final.QuadPart - qpc_initial.QuadPart;
                    qpc_delta_ns = qpc_delta.QuadPart * 1000000000ll / host->qpc_frequency;
                    #else
                    assertm(false, "The requested timing method is unavailable on this platform.");
                    #endif
                } break;
                case TIMING_QTCT: {
                    #ifdef _WIN32
                    // For now, we assume that QueryThreadCycleTime() internally uses RDTSC,
                    // so we can treat it the same way. Note that this will NOT be true
                    // on ARM platforms.
                    QueryThreadCycleTime(current_thread, &tsc_initial);
                    target(result.input, n, &rand_state_local, scratch.a);
                    QueryThreadCycleTime(current_thread, &tsc_final);
                    tsc_delta = tsc_final - tsc_initial;
                    #else
                    assertm(false, "The requested timing method is unavailable on this platform.");
                    #endif
                } break;
                case TIMING_QPCT: {
                    #ifdef _WIN32
                    // We assume that QueryProcessCycleTime() internally uses RDTSC,
                    // so we can treat it the same way.
                    QueryProcessCycleTime(current_process, &tsc_initial);
                    target(result.input, n, &rand_state_local, scratch.a);
                    QueryProcessCycleTime(current_process, &tsc_final);
                    tsc_delta = tsc_final - tsc_initial;
                    #else
                    assertm(false, "The requested timing method is unavailable on this platform.");
                    #endif
                } break;
                default: {
                    assertm(false, "The requested timing method is unimplemented.");
                } break;
                };

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

                // Write time deltas to profiler data, converting to wall time if needed.

                switch(params.timing) {
                case TIMING_RDTSC:
                case TIMING_QTCT:
                case TIMING_QPCT: {
                    // Calculate timings in nanoseconds.
                    f64 tsc_delta_ns = (f64)(tsc_delta * tsc_period_ns);
                    if (rep == 0) {
                        result.units[n_idx * sample_size + i].time = tsc_delta_ns;
                    } else {
                        f64 best_so_far = result.units[n_idx * sample_size + i].time;
                        result.units[n_idx * sample_size + i].time = MIN(tsc_delta_ns, best_so_far);
                    }
                } break;
                case TIMING_QPC: {
                    #ifdef _WIN32
                    if (rep == 0) {
                        result.units[n_idx * sample_size + i].time =
                            (f64)qpc_delta_ns;
                    } else {
                        f64 best_so_far = result.units[n_idx * sample_size + i].time;
                        result.units[n_idx * sample_size + i].time =
                            MIN((f64)qpc_delta_ns, best_so_far);
                    }
                    #else
                    assertm(false, "The requested timing method is unavailable on this platform.");
                    result.units[n_idx * sample_size + i].time = 0.0f;
                    #endif
                } break;
                default: {
                    assertm(false, "The requested timing method is unimplemented.");
                } break;
                };
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
