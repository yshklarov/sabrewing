#include "imgui.h"
#define WIN32_LEAN_AND_MEAN    // Exclude rarely-used definitions.
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "implot.h"
#include <d3d9.h>
#include <intrin.h>
#include <stdint.h>
//#include <tchar.h>
#include <thread>

#include "util.c"
#include "cpuinfo.c"
#include "logger.c"

// TODO Compiler optimization options for targets: Make optimization flags user-selectable.
#include "sort.c"


/**** Constants ****/

#define GLOBAL_ARENA_SIZE 1024*1024*10


/**** Types ****/

// TODO Add timing method: GetTickCount64() (very coarse: 10-16 ms).
// TODO Add timing method: Linux/UNIX CLOCK_MONTONIC_RAW.
typedef enum
{
    TIMING_RDTSC,
    TIMING_QPC,
    TIMING_QTCT,
    TIMING_QPCT,
    TIMING_METHOD_MAX
} timing_method;
static char const * timing_method_str[TIMING_METHOD_MAX] = {
    "RDTSC", "QPC", "QTCT", "QPCT"
};
static char const * timing_method_longstr[TIMING_METHOD_MAX] = {
    "X86 Time Stamp Counter (TSC)",
    "Win32 QueryPerformanceCounter (QPC)",
    "Win32 QueryThreadCycleTime (QTCT)",
    "Win32 QueryProcessCycleTime (QPCT)"
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

// TODO Create a "profiler.c"; separate all profiler code out, away from GUI.
//      (Requires: cross-platform timing.)
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
    timing_method timing;
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

typedef struct
{
    bool visible_imgui_demo_window;
    bool visible_implot_demo_window;
    bool visible_imgui_metrics_window;
    bool visible_log_window;
    bool visible_data_individual;
    bool visible_data_mean;
    bool visible_data_median;
    bool visible_data_bounds;
    bool auto_zoom;
    bool log_show_timestamps;
} gui_config;

typedef struct
{
    bool is_dark;
    u8 font_size_intent;
    u8 font_size;
    u8 font_size_min;
    u8 font_size_max;
} gui_style;

/**** Data ****/

static LPDIRECT3D9              g_pD3D = nullptr;
static LPDIRECT3DDEVICE9        g_pd3dDevice = nullptr;
static bool                     g_DeviceLost = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static D3DPRESENT_PARAMETERS    g_d3dpp = {};


/**** Forward declarations ****/

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void ResetDevice();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


/****  Functions ****/

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
// This will return more accurate successive estimates if it's called more than once.
// The first time it is called, it will sleep some (very short) amount of time.
f32 measure_tsc_period()
{
    // TODO Eliminate these global variables -- they should be passed in as part of a host_info.
    static bool already_called = false;
    static u64 tsc_initial = 0;
    static LARGE_INTEGER qpc_initial = {0};
    static LARGE_INTEGER qpc_frequency = {0};
    if (!already_called) {
        QueryPerformanceFrequency(&qpc_frequency);
        QueryPerformanceCounter(&qpc_initial);
        tsc_initial = __rdtsc();
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(1ms);
        already_called = true;
    }

    u64 tsc_delta = 0;  // TSC units elapsed.
    LARGE_INTEGER qpc_delta = {0};
    f32 qpc_delta_s = 0;  // Seconds elapsed.

    // TODO Fences (see https://stackoverflow.com/a/12634857/1989005)
    u64 tsc_now = __rdtsc();
    tsc_delta = MAX(1ll, tsc_now - tsc_initial);
    LARGE_INTEGER qpc_now = {0};
    QueryPerformanceCounter(&qpc_now);
    qpc_delta.QuadPart = qpc_now.QuadPart - qpc_initial.QuadPart;
    qpc_delta_s = (f32)qpc_delta.QuadPart / qpc_frequency.QuadPart;

    // Reset -- we don't use the full period to measure, in case the TSC period changes (this
    // may happen on older CPUs).
    // Not doing this now, because I don't know that it's needed, and anyway the time between
    // function calls would perhaps be too coarse to get meaningful data.
    //tsc_initial = tsc_now;
    //qpc_initial = qpc_now;

    return qpc_delta_s / tsc_delta;
}

u64 measure_qpc_frequency()
{
    LARGE_INTEGER qpc_frequency = {0};
    QueryPerformanceFrequency(&qpc_frequency);
    return qpc_frequency.QuadPart;
}

// Probe the host for general information about the processor, etc.
// Note: More detailed CPU information is available through Sysinternals Coreinfo.
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
        host->qpc_frequency = measure_qpc_frequency();
        host-> initialized = true;
    }

    // Measure continually, because (on some systems) it may change, and in any case
    // we can get a more precise estimate by measuring over longer time periods.
    host->tsc_period = measure_tsc_period();
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
    LARGE_INTEGER qpc_initial = {0};
    LARGE_INTEGER qpc_final = {0};
    LARGE_INTEGER qpc_delta = {0};
    u64 qpc_delta_ns = 0;  // Unit: Nanoseconds.
    f32 tsc_period_ns = host->tsc_period * 1000000000;

    HANDLE current_thread = GetCurrentThread();
    HANDLE current_process = GetCurrentProcess();

    for (i32 rep = 0; rep < params.repetitions; ++rep) {
        // Each repetition must use the same sample, so we re-seed here.
        rand_state rand_state_local;
        rand_init_from_seed(&rand_state_local, params.seed);

        u64 n_idx = 0;
        // TODO Prettier range loop, perhaps.
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
                    // TODO Fences (see https://stackoverflow.com/a/12634857/1989005)
                    tsc_initial = __rdtsc();
                    target(result.input, n, &rand_state_local, scratch.a);
                    tsc_final = __rdtsc();
                    tsc_delta = tsc_final - tsc_initial;
                } break;
                case TIMING_QPC: {
                    QueryPerformanceCounter(&qpc_initial);
                    target(result.input, n, &rand_state_local, scratch.a);
                    QueryPerformanceCounter(&qpc_final);
                    qpc_delta.QuadPart = qpc_final.QuadPart - qpc_initial.QuadPart;
                    qpc_delta_ns = qpc_delta.QuadPart * 1000000000ll / host->qpc_frequency;
                } break;
                case TIMING_QTCT: {
                    // For now, we assume that QueryThreadCycleTime() internally uses RDTSC,
                    // so we can treat it the same way. Note that this will NOT be true
                    // on ARM platforms.
                    QueryThreadCycleTime(current_thread, &tsc_initial);
                    target(result.input, n, &rand_state_local, scratch.a);
                    QueryThreadCycleTime(current_thread, &tsc_final);
                    tsc_delta = tsc_final - tsc_initial;
                } break;
                case TIMING_QPCT: {
                    // We assume that QueryProcessCycleTime() internally uses RDTSC,
                    // so we can treat it the same way.
                    QueryProcessCycleTime(current_process, &tsc_initial);
                    target(result.input, n, &rand_state_local, scratch.a);
                    QueryProcessCycleTime(current_process, &tsc_final);
                    tsc_delta = tsc_final - tsc_initial;
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
                    f64 qpc_delta_ns_f = (f64)qpc_delta_ns;
                    if (rep == 0) {
                        result.units[n_idx * sample_size + i].time = qpc_delta_ns_f;
                    } else {
                        f64 best_so_far = result.units[n_idx * sample_size + i].time;
                        result.units[n_idx * sample_size + i].time = MIN(qpc_delta_ns_f, best_so_far);
                    }
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
    // TODO Prettier range loop, perhaps.
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


void set_imgui_style(logger* l, ImGuiIO* io, bool is_dark, u8 font_size_pixels)
{
    is_dark ?
        ImGui::StyleColorsDark() :
        ImGui::StyleColorsLight();

    // TODO load multiple fonts and use ImGui::PushFont()/PopFont() to select them (see FONTS.md).
    // TODO AddFontFromFileTTF() returns ImFont*; store it so we can select it later.
    // TODO Use '#define IMGUI_ENABLE_FREETYPE' in imconfig.h to use Freetype for higher quality
    //      font rendering.

    // TODO Detect DPI changes and set style/scaling appropriately. See:
    // https://github.com/ocornut/imgui/blob/master/
    //                 docs/FAQ.md#q-how-should-i-handle-dpi-in-my-application

    io->Fonts->ClearFonts();
    char const * font_filename = "../res/fonts/ClearSans-Regular.ttf";

    ImFont* font = 0;
    // ImGui has a built-in assert for when the file isn't found, so we must check, first.
    if (file_exists(font_filename)) {
        font = io->Fonts->AddFontFromFileTTF(font_filename, (f32)font_size_pixels);
        // Load glyphs for additional symbol codepoints.
        // To see which glyphs a font supports: Use https://fontdrop.info/
        ImFontConfig config;
        config.MergeMode = true;
        // For ImGui, this array's lifetime must persist.
        static const ImWchar extra_ranges[] = {
            // Basic multilingual plane.
            0x0001, 0xFFFF,
            // Higher planes. This is only useful if our font includes these glyphs, and
            // requires defining IMGUI_USE_WCHAR32 in imconfig.h, and a clean rebuild.
            //0x1EC70, 0x1FBFF,  // Additional symbols
            0 };
        io->Fonts->AddFontFromFileTTF(
                "../res/fonts/ClearSans-Regular.ttf",
                (f32)font_size_pixels,
                &config,
                extra_ranges
            );
    }
    // TODO Use an icon font for icons (see FONTS.md): trash can and magnifier for results list.
    if (font == nullptr) {
        logger_appendf(l, LOG_LEVEL_ERROR,
                       "Failed to load font: %s. Falling back on ugly default font.",
                       font_filename);
        font = io->Fonts->AddFontDefault();
        // We have to scale the default font because it is very tiny.
        io->ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;
    }
    //io->Fonts->Build();  // Unnecessary.
    ResetDevice();

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look
    // identical to regular ones.
    ImGuiStyle& style = ImGui::GetStyle();
    if (io->ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    } else {
        style.WindowRounding = font_size_pixels * 0.3f;
    }
}

/**** ImGui assistant functions ****/

// Show tooltip (stolen from imgui_demo.cpp).
static void HelpMarker(const char* desc)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void PushBigButton()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(
                                1 * ImGui::GetFontSize(),
                                0.5f * ImGui::GetFontSize()));
}
static void PopBigButton() { ImGui::PopStyleVar(); }

bool RightAlignedButton(char const * label) {
    f32 width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
    return ImGui::Button(label);
}

/**** Our windows ****/

void show_log_window(
        gui_config* guiconf,
        logger* l)
{
    if (!guiconf->visible_log_window) {
        return;
    }
    ImGui::Begin("Log", &guiconf->visible_log_window);

    if (ImGui::Button("Clear log")) {
        logger_clear(l);
    }
    ImGui::SameLine();
    if (ImGui::Button("Options...")) {
        ImGui::OpenPopup("Logging options");
    }
    if (ImGui::BeginPopup("Logging options", NULL)) {
        ImGui::Checkbox("Show timestamps", &guiconf->log_show_timestamps);
        ImGui::EndPopup();
    }

    // TODO Add search/filter.

    ImGui::Separator();

    if (ImGui::BeginChild("PaddingChild",
                          ImVec2(0, 0 /*-ImGui::GetFrameHeightWithSpacing() * 1 */),
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        // ImGui is very slow at displaying >10,000 separate Text widgets.
        // TODO Pagify ("next/previous page" buttons).
        #define LOG_WINDOW_MAX_ENTRIES_PER_PAGE 1000
        for (u32 i = 0; i < l->len; ++i) {
            ImGui::TextUnformatted(
                    guiconf->log_show_timestamps
                    ? (char*)logger_get_message_with_timestamp(*l, i)
                    : (char*)logger_get_message(*l, i));
        }
        #undef LOG_WINDOW_MAX_ENTRIES_PER_PAGE
        // Autoscroll.
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void show_profiler_windows(
        gui_config* guiconf,
        logger* l,
        arena* a,
        host_info const* host,
        darray_profiler_result* results)
{
    ImGui::Begin("Profiler" /*, visible*/);

    static profiler_params next_run_params = profiler_params_default();

    ImGui::PushItemWidth(ImGui::GetFontSize() * 10);

    if (ImGui::CollapsingHeader("Sampler configuration", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginCombo("Sampler", samplers[next_run_params.sampler_idx].name, 0)) {
            for (int i = 0; i < ARRAY_SIZE(samplers); i++) {
                bool is_selected = (next_run_params.sampler_idx == i);
                if (ImGui::Selectable(samplers[i].name, is_selected)) {
                    next_run_params.sampler_idx = i;
                }
                // Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Text(samplers[next_run_params.sampler_idx].description);
        ImGui::Separator();

        // TODO Implement our own copy ImGui::DragIntRange2, which supports u64 and a third "stride"
        //      parameter in the middle, and does better bounds checking.
        if (ImGui::DragIntRange2(
                    "Array size (n) range", &next_run_params.ns.lower, &next_run_params.ns.upper,
                    20, 0, I32_MAX, "Min: %d", "Max: %d")) {
            // Workarounds for imgui input bugs.
            range_i32_repair(&next_run_params.ns);
        }
        if (ImGui::DragInt(
                    "Array size (n) stride",
                    &next_run_params.ns.stride,
                    1, 1, I32_MAX, "%d",
                    ImGuiSliderFlags_AlwaysClamp)) {
            // I don't trust the ImGui widgets to be bug-free.
            next_run_params.ns.stride = MAX(1, next_run_params.ns.stride);
        }
        ImGui::DragInt(
                "Sample size for each n",
                &next_run_params.sample_size,
                1, 1, I32_MAX, "%d",
                ImGuiSliderFlags_AlwaysClamp);
        i32 sampler_n_count = range_i32_count(next_run_params.ns);
        ImGui::Text(
                u8"The sampler will generate %d × %d = %ld test units.",
                sampler_n_count,
                next_run_params.sample_size,
                // TODO Deal correctly with integer overflow: do not simply crash...
                sampler_n_count * next_run_params.sample_size);
        //ImGui::Text("Input to algorithm: Shuffled array of u32 of length n.");
        ImGui::BeginDisabled(next_run_params.seed_from_time);
        if (next_run_params.seed_from_time) {
            next_run_params.seed = rand_get_seed_from_time();
        }
        ImGui::Separator();
        ImGui::InputScalar(
                "RNG seed", ImGuiDataType_U64, &next_run_params.seed, NULL, NULL, "%llu");
        ImGui::EndDisabled();
        ImGui::Checkbox("Seed with current time", &next_run_params.seed_from_time);
        ImGui::PopItemWidth();
    }

    if (ImGui::CollapsingHeader("Profiler target", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushItemWidth(ImGui::GetFontSize() * 10);
        if (ImGui::BeginCombo("Target", targets[next_run_params.target_idx].name, 0)) {
            for (int i = 0; i < ARRAY_SIZE(targets); i++) {
                bool is_selected = (next_run_params.target_idx == i);
                if (ImGui::Selectable(targets[i].name, is_selected)) {
                    next_run_params.target_idx = i;
                }
                // Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Text(targets[next_run_params.target_idx].description);
        ImGui::PopItemWidth();
    }

    if (ImGui::CollapsingHeader("Profiler options", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushItemWidth(ImGui::GetFontSize() * 3);
        ImGui::DragInt(
                "Repetitions",
                &next_run_params.repetitions,
                1, 1, I32_MAX, "%d",
                ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopItemWidth();
        ImGui::SameLine(); HelpMarker(
                "Perform the test run multiple times, using the same input, storing "
                "only the minimum time measured for each test unit (i.e., for each input). "
                "This is slow, but helps to avoid bad measurements due to thead and process "
                "pre-empting."
                "\n\n"
                "Increase this parameter if you need high-precision measurements, and you observe "
                "poor repeatability across identical test runs. Decrease this parameter if you "
                "are timing a slower algorithm and don't need good repeatability. ");
        i32 sampler_n_count = range_i32_count(next_run_params.ns);
        // TODO Deal with integer overflow: do not simply crash...
        i32 sampler_test_unit_count = sampler_n_count * next_run_params.sample_size;
        ImGui::Text(
                u8"The target will be invoked %d × %d = %d times.",
                sampler_test_unit_count,
                next_run_params.repetitions,
                sampler_test_unit_count * next_run_params.repetitions);

        ImGui::Separator();

        ImGui::Text("Timing method:");
        ImGui::SameLine(); HelpMarker(
                "If in doubt, use QPC, as it gives the most reliable wall time interval. "
                "\n\n"
                "If you require a very fine time resolution (finer than the QPC period), pick "
                "RDTSC. This will only work on x86 platforms, and may give unreliable "
                "data on very old CPUs with non-invariant TSC registers."
                "\n\n"
                "Both QTCT and QPCT often use RDTSC internally, but when the thread (resp. "
                "process) is pre-empted they compensate by subtracting. Note that QPCT gives the "
                "_sum_ of timings of all threads in the current process, including those unrelated "
                "to the profiler target, so its output data will be higher. These two methods "
                "may fail to convert to accurate wall time. "
            );
        for (i32 i = 0; i < TIMING_METHOD_MAX; ++i) {
            if (ImGui::RadioButton(timing_method_longstr[i],
                                   (i32)next_run_params.timing == i)) {
                next_run_params.timing = (timing_method)i;
            }
        }

        ImGui::Separator();

        ImGui::Text("Timer information:");
        if (ImGui::BeginTable("TimerInfo", 4, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TableSetColumnIndex(1); ImGui::Text("Period");
            ImGui::TableSetColumnIndex(2); ImGui::Text("Frequency");

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("QPC:");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.0f ns", 1.0e9f / host->qpc_frequency);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f MHz", host->qpc_frequency / 1000000.0f);
            ImGui::TableSetColumnIndex(3); HelpMarker(
                "QueryPerformanceCounter() is a Win32 API function that is meant to give a "
                "reliable wall clock interval measurement. Internally, it may use the TSC or "
                "whatever other timing facilities are available on the hardware platform.");

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("TSC:");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f ns", host->tsc_period * 1000000000);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.0f MHz", 0.000001f / host->tsc_period);
            ImGui::TableSetColumnIndex(3); HelpMarker(
                "Time Stamp Counter (TSC) units are (approximately) CPU clock cycles, so on a "
                "4.0 GHz CPU a TSC unit is around 1/4 ns. The TSC units may be slightly shorter "
                "than core crystal clock cycles. On very old CPUs, TSC units correspond exactly to "
                "CPU cycles, whereas modern CPUs have an \"Invariant TSC\" that runs at a constant "
                "frequency, independent of dynamic frequency scaling.");

            ImGui::EndTable();
        }

        ImGui::Separator();

        ImGui::Checkbox("Verify correctness of target output", &next_run_params.verify_correctness);
        ImGui::SameLine(); HelpMarker(
                "Verification involves a simple checksum, and may (occasionally) give false "
                "positives.");
    }

    if (ImGui::CollapsingHeader("Processor information", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("CPUInfo", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("Processor:");
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", host->cpu_name);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("Cache sizes:");
            ImGui::TableSetColumnIndex(1); ImGui::Text(
                    "L1: %u KiB, L2: %u KiB, L3: %u KiB",
                    host->cpu_cache_l1 >> 10,
                    host->cpu_cache_l2 >> 10,
                    host->cpu_cache_l3 >> 10);
            ImGui::SameLine(); HelpMarker(
                    "The total of all data and unified cache accessible to a single core. ");

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("Cores:");
            ImGui::TableSetColumnIndex(1); ImGui::Text("%u", host->cpu_num_cores);
            ImGui::SameLine(); HelpMarker(
                    "The number of logical processors available to the operating system. "
                    "This may differ from the number of physical cores.");

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("Has TSC:");
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", host->has_tsc ? "Yes" : "No");
            ImGui::SameLine(); HelpMarker(
                    "The Time Stamp Counter, present on all x86 CPUs since the i586, is a 64-bit "
                    "register that serves to provide highly precise timing information. In early "
                    "CPUs, it was incremented on each clock cycle.");

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("Invariant TSC:");
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", host->has_invariant_tsc ? "Yes" : "No");
            ImGui::SameLine(); HelpMarker(
                    "In modern x86 CPUs (since 2008), the TSC register increments at a constant "
                    "frequency, independent of per-core dynamic frequency scaling, and is "
                    "synchronized across all cores.");

            ImGui::EndTable();
        }
    }

    PushBigButton();
    bool go_requested = ImGui::Button("Go!");
    PopBigButton();
    if (go_requested) {
        profiler_result result = profiler_execute(l, next_run_params, host);
        if (!result.id){
            logger_append(l, LOG_LEVEL_ERROR, "Failed to run profiler.");
        } else {
            *darray_profiler_result_push(a, results) = result;
            if (guiconf->auto_zoom) {
                // Set plot axes to fit the bounds of the new data.
                ImPlot::SetNextAxesToFit();
            }
        }
    }

    ImGui::End();  // Profiler window

    // TODO Refactor: Move these secondary ImGui windows (profiler result list; profiler plot) into
    // own helper functions.

    ImGui::Begin("Profiler Runs");
    static bool visible_display_options = true;
    // TODO Ewww! Do layout better.
    ImGui::BeginChild("ProfilerRunsChild",
                      ImVec2(0, -(2 + (visible_display_options ? 5 : 0)) *
                             ImGui::GetFrameHeightWithSpacing()));
    {
        if (ImGui::BeginTable("ResultsList", 3)) {
            ImGui::TableSetupColumn("ResultName", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("ResultDetails", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("DeleteResult", ImGuiTableColumnFlags_WidthFixed);
            // TODO A "global" column with global checkbox and global magnifier
            // TODO Drag & drop results to reorder; support multi-select drag & drop.

            bool delete_requested = false;
            u64 delete_idx = 0;
            for (usize i = 0; i < results->len; ++i) {
                profiler_result* result = &(results->data[i]);  // For brevity
                // The ImGui ID must be tied to the actual result, because the results may get
                // deleted and/or reordered, and we want the GUI status (e.g., which treenodes
                // are open) to persist.
                ImGui::PushID((i32)result->id);
                char const* result_name = targets[result->params.target_idx].name;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Checkbox("", &result->plot_visible);
                ImGui::SameLine();
                ImGui::TableSetColumnIndex(1);
                if (result->verification_failure_count > 0) {
                    ImU32 badness_color = (ImU32)(0x600000FF);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, badness_color);
                }
                if (ImGui::TreeNodeEx(result_name,
                                      ImGuiTreeNodeFlags_SpanAllColumns |
                                      ImGuiTreeNodeFlags_SpanAvailWidth |
                                      ImGuiTreeNodeFlags_AllowOverlap
                        )) {
                    profiler_params* p = &result->params;
                    ImGui::Text("Sampler: %s", samplers[p->sampler_idx].name);
                    ImGui::Text("Range: (%d, %d, %d)", p->ns.lower, p->ns.stride, p->ns.upper);
                    ImGui::Text("Sample size: %d", p->sample_size);
                    ImGui::Text("Total units: %d", result->len_units);
                    ImGui::Text("Seed: %llu", p->seed);
                    ImGui::Text("Timing: %s", timing_method_str[p->timing]);
                    ImGui::Text("Repetitions: %d", p->repetitions);
                    ImGui::Text("Output verification: %s", p->verify_correctness
                                ? (0 == result->verification_failure_count
                                   ? "Succeeded"
                                   : "Failed")
                                : "Off");
                    // TODO Display more details:
                    //    - Total memory used by this result (i.e., size of local_arena)
                    //    - Timestamp: Began, finished
                    //    - Total time elapsed
                    //    - Profiling progress bar, with "pause"/"continue" button; hide results by
                    //      default while profiling, but allow user to manually select checkbox to
                    //      view live results in graph as they arrive.
                    if (ImGui::Button("Load these parameters")) {
                        next_run_params = *p;
                    }
                    ImGui::TreePop();
                }
                ImGui::TableSetColumnIndex(2);
                // TODO Table column with zoom magnifier. See: Icon fonts (ImGui: FONTS.md);
                //      Zooming to fit will require storing (double min/max for each axis)
                //      in the struct profiler_result.
                // TODO Closed/open Trash / wastepaper basket icon. See: Icon fonts (ImGui: FONTS.md).
                if (ImGui::SmallButton("×")) {
                    // TODO Confirm: Change icon to "open trash" and require double-press.
                    // Queue for later, so we don't invalidate the loop.
                    delete_requested = true;
                    delete_idx = i;
                }
                ImGui::PopID();
            }
            if (delete_requested) {
                logger_appendf(l, LOG_LEVEL_DEBUG,
                               "Destroying result ID %d.", results->data[delete_idx].id);
                result_destroy(&(results->data[delete_idx]));
                darray_profiler_result_remove(results, delete_idx);
            }
            ImGui::EndTable();
        }
    }

    ImGui::EndChild();

    {
        ImGui::BeginDisabled(results->len == 0);
        if (RightAlignedButton("Clear all data")) {
            ImGui::OpenPopup("Clear all data");
        }
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("Clear all data", NULL)) {
            if (results->len == 0) {
                // User already cleared the data in some other way.
                ImGui::CloseCurrentPopup();
            }
            ImVec2 popup_button_size = ImVec2(ImGui::GetFontSize() * 3.5f, 0.f);
            if (ImGui::Button("Confirm", popup_button_size)) {
                logger_appendf(l, LOG_LEVEL_DEBUG, "Destroying %d %s.", results->len,
                               (results->len == 1) ? "result" : "results");
                for (usize i = 0; i < results->len; ++i) {
                    result_destroy(&(results->data[i]));
                }
                darray_profiler_result_clear(results);
                //requested_clear_all = false;
            }
            if (ImGui::Button("Cancel", popup_button_size)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // TODO For details on how to plot heterogeneous data, see implot.h:806.
    visible_display_options = ImGui::CollapsingHeader(
                "Display options",
                ImGuiTreeNodeFlags_DefaultOpen);
    if (visible_display_options) {
        bool visible_any_prev =
            guiconf->visible_data_individual ||
            guiconf->visible_data_mean ||
            guiconf->visible_data_median ||
            guiconf->visible_data_bounds;
        ImGui::Checkbox("Display individual test units", &guiconf->visible_data_individual);
        ImGui::Checkbox("Display bounds", &guiconf->visible_data_bounds);
        ImGui::Checkbox("Display median", &guiconf->visible_data_median);
        ImGui::Checkbox("Display mean", &guiconf->visible_data_mean);
        ImGui::Checkbox("Auto-zoom", &guiconf->auto_zoom);
        bool visible_any_now =
            guiconf->visible_data_individual ||
            guiconf->visible_data_mean ||
            guiconf->visible_data_median ||
            guiconf->visible_data_bounds;
        if (!visible_any_prev && visible_any_now) {
            // Bugfix: If user made new data while nothing was visible, we must re-adjust axes.
            ImPlot::SetNextAxesToFit();
        }
    }

    ImGui::End();  // Window: Profiler Runs


    ImGui::Begin("Running Time"/*, visible*/);

    ImGui::BeginChild("PlotChild",
                ImVec2(0, -1),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                      ImGuiWindowFlags_None);
    if (ImPlot::BeginPlot(
                "##RunningTime",
                ImVec2(-1, -1),
                ImPlotFlags_Crosshairs
            )) {

        // TODO Allow user to select between nanoseconds and TSC units -- if I can come up with
        //      a reasonable thing to do when two plots are shown together with different units.
        char const* time_axis_label = "Time (ns)";
        ImPlot::SetupAxes("n", time_axis_label, 0, 0);

        for (usize i = 0; i < results->len; ++i) {
            profiler_result* result = &(results->data[i]);
            if (!result->plot_visible) {
                continue;
            }

            // TODO Why is phantom data flickering in the plot, even right at the beginning when
            // result.len_units == 0 ?! Is this a graphics glitch on my system only?
            // TODO Verify len_groups,len_units <= I32_MAX before plotting, due to
            //      ImPlot limitations.
            char const* plot_name = targets[result->params.target_idx].name;

            // TODO Also allow user to pick arbitrary quantiles as upper/lower.
            if (guiconf->visible_data_bounds) {
                ImPlot::PushStyleVar(ImPlotStyleVar_FillAlpha, 0.25f);
                ImPlot::PlotShaded(
                        plot_name,
                        &result->groups[0].n,
                        &result->groups[0].time_min,
                        &result->groups[0].time_max,
                        (i32)result->len_groups,
                        0,
                        0,
                        sizeof(*result->groups));
                ImPlot::PopStyleVar();
            }

            // TODO Line weight should be DPI-independent, not simply a fixed pixel count.
            if (guiconf->visible_data_median) {
                ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, 2.0f);
                ImPlot::PlotLine(
                        plot_name,
                        &result->groups[0].n,
                        &result->groups[0].time_median,
                        (i32)result->len_groups,
                        0,
                        0,
                        sizeof(*result->groups));
            }

            if (guiconf->visible_data_mean) {
                ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, 2.0f);
                ImPlot::PlotLine(
                        plot_name,
                        &result->groups[0].n,
                        &result->groups[0].time_mean,
                        (i32)result->len_groups,
                        0,
                        0,
                        sizeof(*result->groups));
            }

            if (guiconf->visible_data_individual) {
                // TODO This gets slow when there are more than 10-30,000 points. Either (easiest)
                // resample (only plot a subset of points), or implement our own PlotScatter that
                // doesn't use ImDrawList but instead renders directly (with a shader?), or find
                // some other solution.

                // NOTE ImPlotMarker_Circle looks nicer than ImPlotMarker_Cross, but 3 times slower.
                // (The marker geometry is described in implot_items.cpp).
                /*
                ImPlot::SetNextFillStyle(
                        IMPLOT_AUTO_COL, 0.3f);
                ImPlot::SetNextMarkerStyle(
                        ImPlotMarker_Circle, ImGui::GetFontSize() * 0.12f,
                        IMPLOT_AUTO_COL, IMPLOT_AUTO, IMPLOT_AUTO_COL);
                */
                ImPlot::SetNextMarkerStyle(
                        ImPlotMarker_Cross, ImGui::GetFontSize() * 0.2f,
                        IMPLOT_AUTO_COL, IMPLOT_AUTO, IMPLOT_AUTO_COL);
                ImPlot::PlotScatter(
                        plot_name,
                        &result->units[0].n,
                        &result->units[0].time,
                        (i32)result->len_units,
                        0,
                        0,
                        sizeof(*result->units)
                    );
            }
        }  // for (result ...)
        ImPlot::EndPlot();
    }
    ImGui::EndChild();

    ImGui::End();   // Window: Profiler Plot
}

int main(int, char**)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr),
        nullptr, nullptr, nullptr, nullptr, L"WorkshopMain", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(
            wc.lpszClassName, L"Algorithm Workshop", WS_OVERLAPPEDWINDOW | WS_MAXIMIZE,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Win32: Show the window.
    // TODO Restore maximized/restored state from before, just like remedybg.
    // See: https://learn.microsoft.com/en-us/windows/
    //         win32/api/winuser/nf-winuser-showwindow?redirectedfrom=MSDN
    ::ShowWindow(hwnd, SW_MAXIMIZE);
    //::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Set up Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Multi-Viewport / Platform Windows work nicely with Win32, but not on Linux.
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    //io.ConfigViewportsNoAutoMerge = true;
    //io.ConfigViewportsNoTaskBarIcon = true;
    // We do NOT set DpiEnableScaleFonts, because it results in blurry text.
    //io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(g_pd3dDevice);

    // User-facing GUI options.
    gui_config guiconf = {0};
    guiconf.visible_imgui_demo_window = true;
    guiconf.visible_implot_demo_window = true;
    guiconf.visible_imgui_metrics_window = true;
    guiconf.visible_log_window = true;
    //guiconf.visible_profiler_window = true;
    guiconf.visible_data_individual = true;
    guiconf.visible_data_mean = true;
    guiconf.visible_data_median = false;
    guiconf.visible_data_bounds = true;
    guiconf.log_show_timestamps = true;
    guiconf.auto_zoom = true;

    // GUI styling/theme.
    gui_style guistyle;
    guistyle.is_dark = false;
    guistyle.font_size_intent = 28;
    guistyle.font_size = 28;
    guistyle.font_size_min = 8;
    guistyle.font_size_max = 60;
    bool guistyle_changed = true;

    // Global state (non-GUI).
    logger global_log = logger_create();
    arena global_arena = arena_create(GLOBAL_ARENA_SIZE);
    host_info host = {0};
    darray_profiler_result profiler_results = darray_profiler_result_new(&global_arena, 5);

    // Main loop
    bool done = false;
    while (!done) {
        query_host_info(&host);

        // Poll and handle messages (inputs, window resize, etc.)
        // See the WndProc() function below for our to dispatch events to the Win32 backend.
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Handle lost D3D9 device
        if (g_DeviceLost) {
            HRESULT hr = g_pd3dDevice->TestCooperativeLevel();
            if (hr == D3DERR_DEVICELOST) {
                ::Sleep(10);
                continue;
            }
            if (hr == D3DERR_DEVICENOTRESET) {
                ResetDevice();
            }
            g_DeviceLost = false;
        }

        // Handle window resize (we don't resize directly in the WM_SIZE handler)
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            g_d3dpp.BackBufferWidth = g_ResizeWidth;
            g_d3dpp.BackBufferHeight = g_ResizeHeight;
            g_ResizeWidth = g_ResizeHeight = 0;  // Clear resize command.
            ResetDevice();
        }

        // Update styles. Note that the font must be set before ImGui::NewFrame().
        // TODO Detect DPI changes and set style/scaling appropriately. See:
        // https://github.com/ocornut/imgui/blob/master/
        //                 docs/FAQ.md#q-how-should-i-handle-dpi-in-my-application
        if (guistyle_changed) {
            set_imgui_style(&global_log, &io, guistyle.is_dark, guistyle.font_size);
            guistyle_changed = false;
        }

        // Start the Dear ImGui frame
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Main menu
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Quit", "Alt+F4")) {
                    done = true;
                }
                ImGui::EndMenu();  // File
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::BeginMenu("Color scheme")) {
                    bool style_light = !guistyle.is_dark;
                    if (ImGui::MenuItem("Light", NULL, &style_light)) {
                        guistyle.is_dark = !style_light;
                        guistyle_changed = true;
                    }
                    if (ImGui::MenuItem("Dark", NULL, &guistyle.is_dark)) {
                        guistyle_changed = true;
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Font")) {
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
                    ImGui::SliderScalar(
                            "UI Font Size (px)",
                            ImGuiDataType_U8,
                            &guistyle.font_size_intent,
                            &guistyle.font_size_min,
                            &guistyle.font_size_max,
                            NULL, ImGuiSliderFlags_AlwaysClamp);
                    // Don't update size immediately, but instead wait until the user releases the
                    // mouse button -- if we updated right away, then there would be ugly GUI
                    // flickering due to the slider element moving around while it's still active.
                    if (!ImGui::IsItemActive() &&
                        guistyle.font_size != guistyle.font_size_intent
                        ) {
                        guistyle.font_size = guistyle.font_size_intent;
                        guistyle_changed = true;
                    }
                    ImGui::EndMenu();  // View->Font
                }
                ImGui::Separator();
                ImGui::MenuItem("Log window", NULL, &guiconf.visible_log_window);
                ImGui::Separator();
                ImGui::MenuItem("ImGui demo window", NULL, &guiconf.visible_imgui_demo_window);
                ImGui::MenuItem("ImPlot demo window", NULL, &guiconf.visible_implot_demo_window);
                ImGui::MenuItem("ImGui metrics window", NULL, &guiconf.visible_imgui_metrics_window);
                //ImGui::MenuItem("Profiler Window", NULL, &guiconf.visible_profiler_window);
                ImGui::EndMenu();  // View
            }
            ImGui::EndMainMenuBar();
        }

        // Main viewport that other viewports can dock to.
        ImGuiViewport const* main_viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(main_viewport->WorkPos);
        ImGui::SetNextWindowSize(main_viewport->WorkSize);
        ImGui::SetNextWindowViewport(main_viewport->ID);
        ImGuiWindowFlags dockspace_flags =
            ImGuiWindowFlags_NoDocking |   // No docking to the host window itself.
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);  // Square corners on dockspace.
        ImGui::Begin("MainDockspace", nullptr, dockspace_flags);
        ImGui::PopStyleVar();
        // We create a separate dockspace hosted within the main viewport. This works better than
        // permitting docking in the main viewport directly.
        ImGuiID main_dockspace_id = ImGui::GetID("MainDockspace");
        ImGui::DockSpace(
                main_dockspace_id,
                ImVec2(0.0f, 0.0f),
                ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::End();

        // ImGui/ImPlot demo/info windows
        if (guiconf.visible_imgui_demo_window)
            ImGui::ShowDemoWindow(&guiconf.visible_imgui_demo_window);
        if (guiconf.visible_implot_demo_window)
            ImPlot::ShowDemoWindow(&guiconf.visible_implot_demo_window);
        if (guiconf.visible_imgui_metrics_window)
            ImGui::ShowMetricsWindow(&guiconf.visible_imgui_metrics_window);

        // Our windows
        show_log_window(&guiconf, &global_log);
        show_profiler_windows(&guiconf, &global_log, &global_arena, &host, &profiler_results);

        // Rendering
        ImGui::EndFrame();

        g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        D3DCOLOR clear_col_dx = D3DCOLOR_RGBA(0, 0, 0, 255);
        g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clear_col_dx, 1.0f, 0);
        if (g_pd3dDevice->BeginScene() >= 0) {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_pd3dDevice->EndScene();
        }

        // Update and Render additional Platform Windows
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        HRESULT result = g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
        if (result == D3DERR_DEVICELOST)
            g_DeviceLost = true;
    }

    // Cleanup
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

/**** Platform helper functions ****/

bool CreateDeviceD3D(HWND hWnd)
{
    if ((g_pD3D = Direct3DCreate9(D3D_SDK_VERSION)) == nullptr)
        return false;

    // Create the D3DDevice
    ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
    g_d3dpp.Windowed = TRUE;
    g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN; // Need to use an explicit format with alpha if needing per-pixel alpha composition.
    g_d3dpp.EnableAutoDepthStencil = TRUE;
    g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
    g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;           // Present with vsync
    //g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;   // Present without vsync, maximum unthrottled framerate
    if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_d3dpp, &g_pd3dDevice) < 0)
        return false;

    return true;
}

void CleanupDeviceD3D()
{
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    if (g_pD3D) { g_pD3D->Release(); g_pD3D = nullptr; }
}

void ResetDevice()
{
    ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
    if (hr == D3DERR_INVALIDCALL)
        IM_ASSERT(0);
    ImGui_ImplDX9_CreateDeviceObjects();
}

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0 // From Windows SDK 8.1+ headers
#endif

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE: {
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    } break;
    case WM_SYSCOMMAND: {
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
    } break;
    case WM_DESTROY: {
        ::PostQuitMessage(0);
        return 0;
    } break;
    case WM_DPICHANGED: {
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DpiEnableScaleViewports) {
            //const int dpi = HIWORD(wParam);
            //printf("WM_DPICHANGED to %d (%.0f%%)\n", dpi, (float)dpi / 96.0f * 100.0f);
            const RECT* suggested_rect = (RECT*)lParam;
            ::SetWindowPos(
                    hWnd, nullptr,
                    suggested_rect->left,
                    suggested_rect->top,
                    suggested_rect->right - suggested_rect->left,
                    suggested_rect->bottom - suggested_rect->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
        }
    } break;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
