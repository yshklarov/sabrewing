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

    u64 _wall_time_freq;
    u64 _wall_time_initial;
    u64 _tsc_initial;
} host_info;

typedef struct
{
    // Sampler parameters
    // NOTE The ns and sample_size are i32 out of necessity for the time being (ImGui standard
    // widgets require it); but they should really be u64 instead.
    range_i32 ns;
    i32 sample_size;
    u64 seed;
    bool seed_from_time;

    // Other parameters
    u32 sampler_idx;
    u32 target_idx;
    bool verifier_enabled;
    u32 verifier_idx;
    bool separate_thread;
    i32 warmup_ms;
    //repeat_method repeat;
    i32 repetitions;
    timing_method_id timing;
    bool adjust_for_timer_overhead;

    // Computed parameters (invariants):
    // num_groups == range_count(ns).
    u64 num_groups;
    // num_units == num_groups * sample_size, or 0 in case of integer overflow.
    u64 num_units;
}  profiler_params;

typedef struct
{
    rand_state seed;  // (sizeof u64)*4 = 32 bytes
    f64 n;
    f64 time;  // nanoseconds
    // Tracking this so the input may be re-created at user's request.
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
    bool valid;
    arena local_arena;  // Memory container; lifetime is until this result is freed.

    // NOTE The sizes of these arrays are defined in struct profiler_params; it is stored separately
    // to simplify thread safety enforcement: the profiler thread gets exclusive access to the
    // profiler_result while it's working, and the GUI thread can in the meantime still access the
    // profiler_params.

    // NOTE All members that may be written during the lifetime of the profiler_result are
    // pointers. This is so that a profiler_result object can be moved by the GUI thread without
    // invalidating the copy held by the profiler thread.

    u32* input;  // Scratch space for storing input to targets.
    u32* input_clone;  // For verifier.

    profiler_result_unit* units;
    profiler_result_group* groups;

    f32* progress;  // Between 0 and 1.
    u64* verification_accept_count;
} profiler_result;

typedef struct
{
    THREAD_EVENT abort_event;
    THREAD_MUTEX result_mutex;
} profiler_sync;


/**** Functions ****/

void profiler_params_recompute_invariants(profiler_params* params) {
    params->num_groups = range_i32_count(params->ns);
    // Check for integer overflow.
    if ((u64)params->num_groups * (u64)params->sample_size <= (u64)I32_MAX) {
        params->num_units = params->num_groups * params->sample_size;
    } else {
        params->num_units = 0;
    }
}

bool profiler_params_valid(profiler_params params) {
    return params.num_units > 0;
}

profiler_params profiler_params_default()
{
    profiler_params params;

    params.ns.lower = 0;
    params.ns.stride = 1;
    params.ns.upper = 200;
    params.sample_size = 10;
    params.seed = 0;
    params.seed_from_time = false;

    params.sampler_idx = 0;
    params.target_idx = 0;
    params.verifier_enabled = true;
    params.verifier_idx = 0;
    params.separate_thread = true;
    params.warmup_ms = 100;
    params.repetitions = 20;
    params.timing = TIMING_RDTSC;
    params.adjust_for_timer_overhead = false;

    profiler_params_recompute_invariants(&params);

    return params;
}

void profiler_result_destroy(profiler_result* result);

// Initialize result. On failure, make a stub (return {0}).
//
// If this call succeeds (i.e., returns a non-stub) then the caller must eventually call
// profiler_result_destroy() to release memory associated with the new result.
profiler_result profiler_result_create(profiler_params params)
{
    profiler_result result = {0};
    bool clone_input = params.verifier_enabled;
    usize arena_len_required = 0;

    // Check for num_units integer overflow.
    if (params.num_units == 0) {
        goto error_memory;
    }

    // Reserve local memory for the result.

    // Memory for one or two copies of the input.
    arena_len_required += (1 + (u32)clone_input) * params.ns.upper * sizeof(params.ns.upper);
    // Result data.
    arena_len_required += params.num_units * sizeof(profiler_result_unit);
    arena_len_required += params.num_groups * sizeof(profiler_result_group);
    arena_len_required += sizeof(*result.verification_accept_count);
    arena_len_required += sizeof(*result.progress);

    result.local_arena = arena_create(arena_len_required);
    if (!result.local_arena.data) {
        // Failed to allocate memory arena.
        goto error_memory;
    }

    // Initialize the result struct.

    result.input = arena_push_array_zero(&result.local_arena, u32, params.ns.upper);
    if (clone_input) {
        result.input_clone = arena_push_array_zero(&result.local_arena, u32, params.ns.upper);
    } else {
        result.input_clone = NULL;
    }
    result.units = arena_push_array_zero(
            &result.local_arena, profiler_result_unit, params.num_units);
    if (!result.units) goto error_memory;
    result.groups = arena_push_array_zero(
            &result.local_arena, profiler_result_group, params.num_groups);
    if (!result.groups) goto error_memory;

    result.verification_accept_count = (u64*)arena_push_zero(
            &result.local_arena, sizeof(*result.verification_accept_count));
    if (!result.verification_accept_count) goto error_memory;
    result.progress = (f32*)arena_push_zero(
            &result.local_arena, sizeof(*result.progress));
    if (!result.progress) goto error_memory;

    result.valid = true;
    return result;

error_memory:
    profiler_result_destroy(&result);
    return result;
}

void profiler_result_destroy(profiler_result* result)
{
    arena_destroy(&result->local_arena);
    static profiler_result empty_result = {0};
    *result = empty_result;
    result->valid = false;  // For good measure.
}

// This function must be called twice (with some time separation) before it will actually set the
// TSC frequency to a nonzero value.
//
// If host->invariant_tsc == true, it will provide more accurate successive estimates each time. If
// invariant_tsc == false, it will only use the last two times it was called as an interval for
// measurement.
//
void update_tsc_frequency(host_info* host, bool first_call)
{
    // Measure the TSC frequency by calling out to another (monotonic, high-resolution) OS-provided
    // wall timer. Note that it's not possible to do this in any other way (except by manually
    // building a comprehensive database of CPU models) because the majority of x86 CPUs do not have
    // instructions to provide this data.
    //
    // Note: It would also possible to fetch the TSC frequency from the Linux kernel, but we
    // don't need so much precision because we're free to measure over a longer time interval.

    if (first_call) {
        host->_wall_time_initial = get_ostime_count(!host->has_invariant_tsc);
        host->_wall_time_freq = get_ostime_freq();
        _mm_lfence();
        host->_tsc_initial = __rdtsc();
        return;
    }

    u64 wall_time_now = get_ostime_count(!host->has_invariant_tsc);
    _mm_lfence();
    u64 tsc_now = __rdtsc();

    u64 wall_time_elapsed = wall_time_now - host->_wall_time_initial;
    u64 tsc_elapsed = tsc_now - host->_tsc_initial;
    // Floating-point arithmetic here is the safest way to avoid integer overflow.
    host->tsc_frequency = (u64)(0.5f + host->_wall_time_freq *
           ((f32)tsc_elapsed / (f32)(MAX(1ll, wall_time_elapsed))));

    if (!host->has_invariant_tsc) {
        // Discard previous data, because the frequency may be changing.
        host->_tsc_initial = tsc_now;
        host->_wall_time_initial = wall_time_now;
    }
}

u64 get_qpc_frequency()
{
    #ifdef _WIN32
    // The QPC frequency is fixed at system boot, and guaranteed not to change.
    static LARGE_INTEGER qpc_frequency = {0};
    if (!qpc_frequency.QuadPart) {
        QueryPerformanceFrequency(&qpc_frequency);
    }
    return qpc_frequency.QuadPart;
    #else
    return 0;
    #endif
}

// Return the value of the given timer.
/* We prevent inlining because the compiler might inline some calls and not others, which would
   interfere with timings (e.g., adjust_for_timer_overhead would be spoiled). */
NEVER_INLINE
u64 get_timer_value(timing_method_id tmid)
{
    // This is branchy, but not overly so. If the user cares about the extra dozen instructions due
    // to calling this function (as opposed to calling, say, __rdtsc() directly), they can adjust by
    // subtracting get_timer_overhead().

    // Another way to do this more efficiently would be to re-structure: Move the switch statment to
    // the target call, instead calling the target inside each case. The rdtsc could be done with
    // inline asm. But this is all very much overkill for our application, at least for the time
    // being.

    switch(tmid) {
    case TIMING_RDTSC: {
        // The fence is probably unnecessary because we're inside our own stack frame, but we insert
        // in anyway for good measure.
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
        THREAD current_thread = GetCurrentThread();
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
        PROCESS current_process = GetCurrentProcess();
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
        return host->tsc_frequency;
    } break;
    case TIMING_QPC: {
        #ifdef _WIN32
        return host->qpc_frequency;
        #else
        assertm(false, "The requested timing method is unimplemented.");
        return 0;
        #endif
    } break;
    case TIMING_QTCT:
    case TIMING_QPCT: {
        #ifdef _WIN32
        // MSDN doesn't recommend this. But we have to do it somehow, and this seems to work.
        return host->tsc_frequency;
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

// Get the overhead resulting from using the timer: time between successive calls. This function
// tests a large number of repetitions to get a good measurement.
// Warning: The CPU should be "warmed up" when calling this, to get up to its full (or boost)
// frequency; otherwise, the return value may be an overestimate of the true overhead.
u64 get_timer_overhead(timing_method_id tmid, u32 timeout_ms)
{
    // Our process might be pre-empted, so we need to do this many times to be very sure that we
    // don't over-estimate the overhead.

    // This is stupid, but it works -- for most timing methods. Except QueryProcessCycleTime is
    // finicky and unreliable; trying to measure it gives extremely unpredictable results. Still, we
    // allow it, in case the user really wants it.

    // Note also: Some timing methods (QPCT!) are simply inconsistent in how long they take; still,
    // we look for the *minimum* time, because it would very bad to over-estimate.

    u64 start_time = get_ostime_count(false);
    u64 end_time = start_time + get_ostime_freq() * timeout_ms / 1000;
    u64 min_overhead = U64_MAX;
    do {
        u64 one = get_timer_value(tmid);
        u64 two = get_timer_value(tmid);
        u64 overhead = two - one;
        min_overhead = MIN(overhead, min_overhead);
    } while (get_ostime_count(false) < end_time);
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
        // QPC frequency is fixed at system boot.
        host->qpc_frequency = get_qpc_frequency();
    }

    // Measure continually, because (on some systems) it may change, and in any case
    // we can get a more precise estimate by measuring over longer time periods.
    update_tsc_frequency(host, !host->initialized);
    host->initialized = true;
}

void waste_cpu_time(u32 timeout_ms) {
    if (timeout_ms == 0) {
        return;
    }
    u64 start_time = get_ostime_count(false);
    u64 end_time = start_time + get_ostime_freq() * timeout_ms / 1000;
    rand_state rng = {0};
    rand_init_from_time(&rng);
    // The "extra" bit shall prevent the compiler from optimizing away our code.
    u64 extra = 0;
    while(get_ostime_count(false) < (end_time + extra)) {
        for (u32 i = 0; i < 500; ++i) {
            extra = rand_raw(&rng) % 2;
        }
    }
}


void profiler_execute(
        profiler_params params,
        profiler_result result,
        host_info host,
        profiler_sync sync)
{
    f64 timer_period_ns = 1.0e9 / (f64)get_timer_frequency(params.timing, &host);

    u64 sample_size = params.sample_size;  // For brevity.
    fn_sampler_sort sampler = samplers[params.sampler_idx].fn;
    fn_target_sort target = targets[params.target_idx].fn;
    fn_verifier_sort verifier = verifiers[params.verifier_idx].fn;

    // The warmup must precede the call to get_timer_overhead().
    waste_cpu_time(params.warmup_ms);

    // It would be nice to re-measure the overhead for every call of the target, just in case
    // the overhead is changing (with CPU scaling, system load, etc.); however, if we do that,
    // sometimes measuring the overhead gives too high a value (due to thread/process
    // pre-empting or other OS scheduler shenanigans). So, for now, we only measure once.
    u64 timer_overhead =
        params.adjust_for_timer_overhead
        ? get_timer_overhead(params.timing, 1)
        : 0;

    // The verifier must use its own RNG state, independent from the target, because
    // target behaviour should be consistent across repetitions, and across distinct runs
    // regardless of whether a verifier is enabled.
    rand_state rand_state_verifier = {0};
    if (params.verifier_enabled) {
        rand_init_from_time(&rand_state_verifier);
    }

    u64 invocations_completed = 0;
    u64 invocations_total = params.num_units * params.repetitions;

    bool aborting = false;
    for (i32 rep = 0; rep < params.repetitions; ++rep) {
        if (aborting) break;

        // Each repetition must use the same sample, so we re-seed here.
        rand_state rand_state_local = {0};
        rand_init_from_seed(&rand_state_local, params.seed);

        loop_over_range_i32(params.ns, n, n_idx) {
            if (aborting) break;

            for (u64 i = 0; i < (u64)sample_size; ++i) {
                if (aborting) break;

                if (params.separate_thread) {
                    mutex_acquire(sync.result_mutex);
                    if (event_check(sync.abort_event)) {
                        aborting = true;
                        continue;
                    }
                }

                arena_tmp scratch = scratch_get(NULL, 0);
                result.units[n_idx * sample_size + i].n = (f64)n;
                result.units[n_idx * sample_size + i].seed = rand_state_local;

                // Generate input data for this test unit. We do this inside the loop, just before
                // measuring, to encourage the input data to already be in CPU cache when the
                // critical code begins.
                sampler(result.input, n, &rand_state_local, scratch.a);
                if (params.verifier_enabled) {
                    memcpy(result.input_clone, result.input, n * sizeof(*result.input));
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

                // Convert to wall time.
                f64 timer_delta_ns = (f64)timer_delta * timer_period_ns;

                // Save to result data.
                if (rep == 0) {
                    result.units[n_idx * sample_size + i].time = timer_delta_ns;
                } else {
                    f64 best_so_far = result.units[n_idx * sample_size + i].time;
                    result.units[n_idx * sample_size + i].time = MIN(timer_delta_ns, best_so_far);
                }

                // Verify correctness of output.
                if (rep == 0 && params.verifier_enabled) {
                    if (verifier(
                                result.input_clone,  // Input
                                result.input,        // Output (was created in-place by target)
                                n,
                                &rand_state_verifier,
                                scratch.a)) {
                        ++(*result.verification_accept_count);
                    }
                }
                scratch_release(scratch);

                ++invocations_completed;
                *result.progress = (f32)invocations_completed / (f32)invocations_total;
                if (params.separate_thread) {
                    mutex_release(sync.result_mutex);
                }
            }
        }
    } // for (rep ...)

    // Gather result for plotting.
    if (!aborting) {
        if (params.separate_thread) {
            mutex_acquire(sync.result_mutex);
        }

        arena_tmp scratch = scratch_get(0, 0);
        f64* times = arena_push_array_zero(scratch.a, f64, sample_size);
        loop_over_range_i32(params.ns, n, n_idx) {
            result.groups[n_idx].n = (f64)n;
            result.groups[n_idx].time_mean = 0;
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
        }
        scratch_release(scratch);
        if (params.separate_thread) {
            mutex_release(sync.result_mutex);
        }
    }
}

typedef struct {
    profiler_params params;
    profiler_result result;
    host_info host;
    profiler_sync sync;
    THREAD_EVENT done_copying_args;
} profiler_execute_args_struct;

THREAD_ENTRYPOINT profiler_execute_begin(void* vargs)
{
    profiler_execute_args_struct* args = (profiler_execute_args_struct*)vargs;
    profiler_params params = args->params;
    profiler_result result = args->result;
    host_info host = args->host;
    profiler_sync sync = args->sync;
    event_signal(args->done_copying_args);
    profiler_execute(params, result, host, sync);
    return 0;
}
