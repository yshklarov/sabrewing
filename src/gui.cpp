#include "imgui.h"
#include "imgui_internal.h"    // For extending ImGui with custom widgets.
#define WIN32_LEAN_AND_MEAN    // Exclude rarely-used definitions.
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include <SDL.h>
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <SDL_opengles2.h>
#else
#include <SDL_opengl.h>
#endif

#include <inttypes.h>
#include <stdint.h>

#include "Lucide_Symbols.h"

#include "util.c"
#include "util_thread.c"
#include "logger.c"
#include "cpuinfo.c"
#include "problems/sort.c"  // Choose one problem here (compiled in, for now).
#include "profiler.c"


/**** Constants ****/

#define GLOBAL_ARENA_SIZE 1024*1024*10


/**** Types ****/

// The user's settings for the GUI's behavior.
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
    bool live_view;
    bool log_show_timestamps;
} GuiConfig;

// The user's settings for the GUI's styling.
typedef struct
{
    bool is_dark;
    u8 font_size_intent;
    u8 font_size;
    u8 font_size_min;
    u8 font_size_max;
} GuiStyle;

// The state of a single profiler run.
typedef enum
{
    PROFRUN_PENDING,       // The user has queued the run, and it has been created, but the profiler has
                           // not yet begun executing it.
    PROFRUN_RUNNING,       // The profiler is currently executing the run normally.
    PROFRUN_ABORT_REQD,    // The user has commanded an abort in the middle of profiling.
    PROFRUN_ABORTING,      // An abort request has been sent to the profiler.
    PROFRUN_DONE_SUCCESS,  // The profiler has successfully completed and exited.
    PROFRUN_DONE_FAILURE,  // The profiler has encountered a system error and exited. (Note that a
                           // verifier's rejection of a target's output is not considered an error.)
    PROFRUN_DONE_ABORTED,  // The profiler has aborted and exited.
    PROFRUN_STATE_MAX,
} ProfRunState;

// A Profrun (profiler run) stores metadata about a profiler run; it is created as soon as the user
// queues up a run and persists until the user aborts or deletes the run (exposed as a "result" in
// the GUI). The data is accessed through pointers in the ProfilerResult member. This way, the
// Profrun objects can be moved (reordered) by the front-end while the profiler thread/process is
// writing data into the result.
typedef struct
{
    u64 id;  // Unique; 1-indexed; id==0 indicates stub (uninitialized, or already destroyed).
    ProfRunState state;     // Once this is PROFRUN_DONE_..., `result` is safe to read.
    THREAD thread_handle;   // A handle to the profiler thread/process.
    ProfilerSync sync;      // For talking with the profiler thread/process.
    ProfilerParams params;
    ProfilerResult result;  // Written directly by profiler thread: Beware data races.
    bool intent_visible;    // The user wants to visualize the results of this run.
    bool fresh;             // Completed, but result not yet displayed to the user.
} Profrun;
typedef_darray(Profrun, profrun);


/****  Functions ****/

// Return true if the profiler is currently executing the run.
bool profrun_busy(Profrun* run)
{
    return
        run->state == PROFRUN_RUNNING ||
        run->state == PROFRUN_ABORT_REQD ||
        run->state == PROFRUN_ABORTING;
}

// Return true if the profiler has already exited from this run.
bool profrun_done(Profrun* run)
{
    return
        run->state == PROFRUN_DONE_SUCCESS ||
        run->state == PROFRUN_DONE_FAILURE ||
        run->state == PROFRUN_DONE_ABORTED;
}

// Delete the Profrun immediately if possible, or ask the thread to abort if it's running.
// Return true if deleted.
bool profrun_try_delete(Logger* l, darray_profrun* runs, usize idx)
{
    bool deleted = false;
    switch (runs->data[idx].state) {
    case PROFRUN_RUNNING: {
        runs->data[idx].state = PROFRUN_ABORT_REQD;
    } break;
    case PROFRUN_ABORT_REQD:
    case PROFRUN_ABORTING: {
        // Still waiting for thread to abort; do nothing.
    } break;
    default: {
        // No worker thread is running, so it's safe to destroy it immediately.
        logger_appendf(l, LOG_LEVEL_DEBUG,
                       "(ID %" PRIu64 ") Destroying profiler run.",
                       runs->data[idx].id);
        profiler_result_destroy(&(runs->data[idx].result));
        darray_profrun_remove(runs, idx);
        deleted = true;
    }
    }
    return deleted;
}

// Return true if the results for the given run should be plotted.
bool profrun_actually_visible(Profrun* run, bool live_view)
{
    bool data_available =
        live_view ||
        run->state == PROFRUN_DONE_SUCCESS ||
        run->state == PROFRUN_DONE_FAILURE;
    return data_available && run->intent_visible;
}

// Perform some state transitions, logging, and basic cleanup.
// This function should be called after the profiler exits the run.
void profiler_worker_finish(Logger* l, Profrun* run)
{
    if (run->state == PROFRUN_ABORTING) {
        run->state = PROFRUN_DONE_ABORTED;
    } else if (!run->result.valid){
        logger_append(l, LOG_LEVEL_ERROR, "Profiler failed to run.");
        run->state = PROFRUN_DONE_FAILURE;
    } else {
        if (run->params.verifier_enabled) {
            if (*(run->result.verification_accept_count) == run->params.num_units) {
                logger_appendf(
                        l, LOG_LEVEL_INFO,
                        "(ID %" PRIu64 ") Verification success: Verifier accepted %u/%u units.",
                        run->id,
                        *(run->result.verification_accept_count),
                        run->params.num_units);
            } else {
                logger_appendf(
                        l, LOG_LEVEL_INFO,
                        "(ID %" PRIu64 ") Verification failure: Verifier accepted %u/%u units.",
                        run->id,
                        *(run->result.verification_accept_count),
                        run->params.num_units);
            }
        }
        logger_appendf(l, LOG_LEVEL_INFO, "(ID %" PRIu64 ") Completed profiler run.", run->id);
        run->fresh = true;
        run->state = PROFRUN_DONE_SUCCESS;
    }
}

// This function should be invoked frequently, to perform bookkeeping on `runs` and their associated
// profiler threads/process(es).
void manage_profiler_workers(
        Logger* l,
        HostInfo* host,
        darray_profrun* runs)
{
    // Only one worker thread at a time for now, because memory (scratch) arenas are not
    // thread-safe.
    u32 workers_available = 1;
    #ifndef _WIN32
    // pthread mutexes and conds must share a single memory location across all threads.
    static pthread_event_t abort_event;
    static pthread_mutex_t result_mutex;
    #endif
    bool state_changed_this_frame = false;

    // Take care of already-running worker(s).
    for (usize i = 0; i < runs->len; ++i) {
        Profrun* run = &runs->data[i];
        if (run->state == PROFRUN_ABORT_REQD) {
            assertm(run->params.separate_thread, "Worker shouldn't have its own thread.");
            logger_appendf(l, LOG_LEVEL_DEBUG,
                           "(ID %" PRIu64 ") Profiler abort requested.", run->id);
            event_signal(run->sync.abort_event);
            run->state = PROFRUN_ABORTING;
            state_changed_this_frame = true;
        }
        if (profrun_busy(run)) {
            assertm(run->params.separate_thread, "Worker shouldn't have its own thread.");
            assertm(workers_available > 0, "Too many profiler workers running.");
            bool run_completed = thread_has_joined(run->thread_handle);
            if (!run_completed) {
                --workers_available;
            } else {
                #ifdef _WIN32
                CloseHandle(run->thread_handle);
                #endif
                event_destroy(run->sync.abort_event);
                mutex_destroy(run->sync.result_mutex);
                profiler_worker_finish(l, run);
                if (run->state == PROFRUN_DONE_ABORTED) {
                    if (profrun_try_delete(l, runs, i)) {
                        --i;
                    }
                }
                state_changed_this_frame = true;
            }
        }
    }

    // Begin new worker(s).
    for (usize i = 0; i < runs->len; ++i) {
        if (!workers_available) {
            break;
        }
        Profrun* run = &runs->data[i];
        if (run->state == PROFRUN_PENDING) {
            if (!run->params.separate_thread && state_changed_this_frame) {
                // Wait for one GUI frame to update the GUI before blocking the thread. This
                // is so that the results list and graph get a chance to update.
                // Do not begin any later runs yet, either (we always go in order queued).
                break;
            }
            logger_appendf(l, LOG_LEVEL_INFO,
                           "(ID %" PRIu64 ") Starting profiler run.", run->id);
            if (run->params.separate_thread) {
                // Create the worker thread.

                // NOTE The platform-specific organization is messy, but we're going to tear
                // this all out when we switch from threads to processes.

                #ifdef _WIN32

                run->sync.abort_event = CreateEvent(NULL, TRUE, FALSE, NULL);
                run->sync.result_mutex = CreateMutex(NULL, FALSE, NULL);
                // Don't leave this stack frame until the child thread is done with it.
                THREAD_EVENT done_copying_args = CreateEvent(NULL, TRUE, FALSE, NULL);
                profiler_execute_args_struct profiler_args = {
                    run->params,
                    run->result,
                    *host,
                    run->sync,
                    done_copying_args
                };
                run->thread_handle = (THREAD)_beginthreadex(
                        NULL, 0, profiler_execute_begin, &profiler_args, CREATE_SUSPENDED, NULL);
                if (run->thread_handle == 0) {
                    logger_appendf(l, LOG_LEVEL_ERROR,
                                   "(ID %" PRIu64 ") Failed to start profiler thread.", run->id);
                    run->state = PROFRUN_DONE_FAILURE;
                } else {
                    // Worker threads are highest priority; the GUI can wait.
                    SetThreadPriority(run->thread_handle, THREAD_PRIORITY_TIME_CRITICAL);
                    ResumeThread(run->thread_handle);
                    // Wait until it's safe for profiler_args to go out of scope.
                    event_wait(profiler_args.done_copying_args);
                    --workers_available;
                    run->state = PROFRUN_RUNNING;
                }
                CloseHandle(profiler_args.done_copying_args);

                #else
                // Linux:

                // Here, unlike in Win32, we can't copy around mutexes and events/conds across
                // threads: each thread must hold a pointer to a single, shared object.
                event_initialize(&abort_event);
                mutex_initialize(&result_mutex);
                run->sync.abort_event = &abort_event;
                run->sync.result_mutex = &result_mutex;
                // Don't leave this stack frame until the child thread is done with it.
                pthread_event_t done_copying_args;
                event_initialize(&done_copying_args);
                profiler_execute_args_struct profiler_args = {
                    run->params,
                    run->result,
                    *host,
                    run->sync,
                    &done_copying_args
                };
                i32 rtn = pthread_create(
                        &run->thread_handle, NULL, profiler_execute_begin, &profiler_args);
                if (rtn != 0) {
                    logger_appendf(l, LOG_LEVEL_ERROR,
                                   "(ID %" PRIu64 ") Failed to start profiler thread.", run->id);
                    run->state = PROFRUN_DONE_FAILURE;
                } else {
                    // On Linux, setting high thread priority on threads requires superuser
                    // permissions, so we skip it.
                    event_wait(&done_copying_args);  // Don't pop stack frame until it's safe.
                    --workers_available;
                    run->state = PROFRUN_RUNNING;
                }

                #endif
            } else {
                // User requested to use GUI thread for the profiler.
                run->state = PROFRUN_RUNNING;
                // This will block until the run is complete.
                profiler_execute(run->params, run->result, *host, run->sync);
                profiler_worker_finish(l, run);
            }
        }
    }
}

// Update the ImGUI style and fonts. This function should be called after the user modifies
// the arguments, between ImGui frames.
void set_imgui_style(Logger* l, ImGuiIO* io, bool is_dark, u8 font_size)
{
    // Don't re-load fonts unless we have to.
    static u8 prev_font_size = 0;
    if (font_size != prev_font_size) {
        prev_font_size = font_size;
        ImGui_ImplOpenGL3_DestroyFontsTexture();  // Don't leak memory.
        io->Fonts->Clear();

        char const * font_filename = "../res/fonts/ClearSans-Regular.ttf";
        // Lucide icon font (https://lucide.dev/icons/)
        char const * icon_font_filename = "../res/fonts/" FONT_ICON_FILE_NAME_LC;

        ImFont* font = 0;
        ImFont* icon_font = 0;

        // ImGui has a built-in assert for when the file isn't found, so we must check, first.
        if (file_exists(font_filename)) {
            font = io->Fonts->AddFontFromFileTTF(font_filename, (f32)font_size);
            // Load glyphs for additional symbol codepoints.
            // To see which glyphs a font supports: Use https://fontdrop.info/
            // For ImGui, this array's lifetime must persist.
            static const ImWchar extra_ranges[] = {
                // Basic multilingual plane.
                0x0001, 0xFFFF,
                // Higher planes. This is only useful if our font includes these glyphs, and
                // requires defining IMGUI_USE_WCHAR32 in imconfig.h, and a clean rebuild.
                //0x1EC70, 0x1FBFF,  // Additional symbols
                0 };

            ImFontConfig config;
            config.MergeMode = true;
            io->Fonts->AddFontFromFileTTF(
                    font_filename, (f32)font_size, &config, extra_ranges
                );
        }
        if (font == 0) {
            logger_appendf(l, LOG_LEVEL_ERROR,
                           "Failed to load font: %s. Falling back on ugly default font.",
                           font_filename);
            font = io->Fonts->AddFontDefault();
            font_size = 13;  // For scaling of icon fonts and other UI elements later.
            // We have to scale the default font because it is very tiny. This can be very ugly,
            // but at least it's readable.
            io->ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;
        }

        if (file_exists(icon_font_filename)) {
            // Use an icon font for icons (see ImGui: FONTS.md).
            f32 icon_scaling = 1.0f;
            ImFontConfig config;
            config.MergeMode = true;
            // Align vertically. Coefficients are specific to the icon font.
            config.GlyphOffset = { 0.0f, (f32)font_size * (0.5f*icon_scaling - 0.3f) };
            // Enforce monospace font.
            config.GlyphMinAdvanceX = (f32)font_size * 1.0f;
            config.GlyphMaxAdvanceX = (f32)font_size * 1.0f;
            static const ImWchar icon_ranges[] = { ICON_MIN_LC, ICON_MAX_LC, 0 };
            icon_font = io->Fonts->AddFontFromFileTTF(
                    icon_font_filename, (f32)font_size * icon_scaling, &config, icon_ranges);
        }
        if (icon_font == 0) {
            logger_appendf(l, LOG_LEVEL_ERROR,
                           "Failed to load icons: %s.",
                           icon_font_filename);
        }

        // ImGui_ImplOpenGL3_CreateFontsTexture();  // Unnecessary; will be called by NewFrame().
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();  // Reset to default, so that ScaleAllSizes() works.
    is_dark ?
        ImGui::StyleColorsDark() :
        ImGui::StyleColorsLight();
    u8 base_font_size = 20;
    style.ScaleAllSizes((f32)font_size / base_font_size);  // Adjust frame thicknesses and spacing.

    if (io->ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        // Get platform windows to look identical to ordinary OS windows.
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    } else {
        style.WindowRounding = font_size * 0.3f;
    }
}

/**** ImGui helper functions and custom widgets ****/

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
                                1.2f * ImGui::GetFrameHeight(),
                                1.0f * ImGui::GetFrameHeight()));
}
static void PopBigButton() { ImGui::PopStyleVar(); }
static f32 GetBigButtonHeightWithSpacing()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(
                                1.2f * ImGui::GetFrameHeight(),
                                1.0f * ImGui::GetFrameHeight()));
    f32 height = ImGui::GetFrameHeightWithSpacing();
    ImGui::PopStyleVar();
    return height;
}

/*bool RightAlignedButton(char const * label)
{
    f32 width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
    return ImGui::Button(label);
}*/

void TextIcon(char const* icon)
{
    f32 length = ImGui::GetFrameHeight();  // Always re-fetch, in case user changed it.
    ImGui::BeginChildFrame(ImGui::GetID(icon),
                      {length, length},
                      ImGuiWindowFlags_NoBackground |
                      ImGuiWindowFlags_NoDecoration |
                      ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted(icon);
    ImGui::EndChild();
}

void TextIconGhost()
{
    ImGui::TextUnformatted("");
}

// Copied from imgui_widgets.cpp and modified for our purposes.
// Guaranteed to clamp to bounds (even on user ctrl-input).
bool ImGuiDragU32(
        const char* label,
        u32* v,
        f32 v_speed = 1.0f,
        u32 v_min = 0, u32 v_max = U32_MAX,
        const char* format = "%u",
        ImGuiSliderFlags flags = 0) {
    flags |= ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_ClampZeroRange;
    bool modified = ImGui::DragScalar(label, ImGuiDataType_U32, v, v_speed,
                                      &v_min, &v_max, format, flags);
    if (modified) {
        // Don't trust ImGui; do it ourselves to be certain.
        *v = CLAMP(*v, v_min, v_max);
    }
    return modified;
}


// Copied from imgui_widgets.cpp and modified for our purposes.
// Guaranteed to leave v_current within bounds, and guaranteed to leave it as a valid range.
bool ImGuiDragRangeWithStride(
        const char* label,
        range_u32* v_current,
        f32 v_speed_bounds = 1.0f, f32 v_speed_stride = 1.0f,
        u32 v_min_bounds = 0, u32 v_max_bounds = 0,
        u32 v_min_stride = 1, u32 v_max_stride = U32_MAX,
        const char* format_lower = "%u",
        const char* format_stride = "%u",
        const char* format_upper = "%u",
        ImGuiSliderFlags flags = 0)
{
    ImGuiStyle& style = ImGui::GetStyle();
    bool value_changed = false;
    flags |= ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_ClampZeroRange;
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());

    if (ImGuiDragU32("##lower", &v_current->lower, v_speed_bounds,
                v_min_bounds, v_current->upper, format_lower, flags)) {
        value_changed = true;
        v_current->upper = MAX(v_current->lower, v_current->upper);
    }
    ImGui::PopItemWidth();
    ImGui::SameLine(0, style.ItemInnerSpacing.x);

    if (ImGuiDragU32("##stride", &v_current->stride, v_speed_stride,
                v_min_stride, v_max_stride, format_stride, flags)) {
        value_changed = true;
    }
    ImGui::PopItemWidth();
    ImGui::SameLine(0, style.ItemInnerSpacing.x);

    if (ImGuiDragU32("##upper", &v_current->upper, v_speed_bounds,
                v_current->lower, v_max_bounds, format_upper, flags)) {
        value_changed = true;
        v_current->lower = MIN(v_current->lower, v_current->upper);
    }
    ImGui::PopItemWidth();
    ImGui::SameLine(0, style.ItemInnerSpacing.x);

    ImGui::TextEx(label, ImGui::FindRenderedTextEnd(label));
    ImGui::EndGroup();
    ImGui::PopID();

    // Don't trust the ImGui widgets; do it ourselves to be very sure.
    range_u32_clamp(v_current, v_min_bounds, v_max_bounds);
    range_u32_repair(v_current);
    return value_changed;
}


/**** Our windows ****/

void show_log_window(
        GuiConfig* guiconf,
        Logger* l)
{
    if (!guiconf->visible_log_window) {
        return;
    }
    ImGui::Begin("Log", &guiconf->visible_log_window);

    if (ImGui::Button(ICON_LC_ERASER " Clear log")) {
        logger_clear(l);
    }
    ImGui::SameLine();
    if (ImGui::Button("Options...")) {
        ImGui::OpenPopup("Logging options");
    }
    if (ImGui::BeginPopup("Logging options", 0)) {
        ImGui::Checkbox("Show timestamps", &guiconf->log_show_timestamps);
        ImGui::EndPopup();
    }

    ImGui::Separator();

    if (ImGui::BeginChild("PaddingChild",
                          ImVec2(0, 0 /*-ImGui::GetFrameHeightWithSpacing() * 1 */),
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
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
        GuiConfig* guiconf,
        Logger* l,
        Arena* a,
        HostInfo* host,
        darray_profrun* runs)
{
    ImGui::Begin("Profiler" /*, visible*/);

    static ProfilerParams next_run_params = profiler_params_default();

    f32 icon_width = ImGui::GetFrameHeightWithSpacing();
    f32 option_width = ImGui::GetFontSize() * 12;

    ImGui::BeginChild("ProfilerParamsConfigurationChild",
                      ImVec2(0, -GetBigButtonHeightWithSpacing()));
    {
    if (ImGui::CollapsingHeader("Problem##Header", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Problem");
        TextIcon(ICON_LC_BOX); ImGui::SameLine(icon_width);
        ImGui::Text("%s", problem_description());
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Sampler##Header", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Sampler");
        TextIcon(ICON_LC_DICES); ImGui::SameLine(icon_width);
        ImGui::PushItemWidth(option_width);
        if (ImGui::BeginCombo("Sampler", samplers[next_run_params.sampler_idx].name, 0)) {
            for (u32 i = 0; i < (u32)ARRAY_SIZE(samplers); i++) {
                bool is_selected = (next_run_params.sampler_idx == i);
                if (ImGui::Selectable(samplers[i].name, is_selected)) {
                    next_run_params.sampler_idx = i;
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        TextIconGhost(); ImGui::SameLine(icon_width);
        ImGui::TextUnformatted(samplers[next_run_params.sampler_idx].description);
        TextIconGhost(); ImGui::SameLine(icon_width);
        ImGui::Text("Output: %s", sampler_output_description());
        ImGui::PopItemWidth();
        ImGui::Separator();

        ImGui::PushItemWidth(option_width);

        TextIcon(ICON_LC_TALLY_5); ImGui::SameLine(icon_width);
        if (ImGuiDragRangeWithStride(
                    "Range for n",
                    &next_run_params.ns,
                    10.0f, 1.0f,
                    0, U32_MAX,
                    1, U32_MAX,
                    "Min: %u",
                    "Stride: %u",
                    "Max: %u")) {
            profiler_params_recompute_invariants(&next_run_params);
        }

        TextIconGhost(); ImGui::SameLine(icon_width);
        if (ImGuiDragU32(
                    "Sample size for each n",
                    &next_run_params.sample_size,
                    1, 1, U32_MAX, "%u",
                    ImGuiSliderFlags_AlwaysClamp)) {
            profiler_params_recompute_invariants(&next_run_params);
        }
        TextIconGhost(); ImGui::SameLine(icon_width);
        ImGui::Text(
                u8"Sampler will be invoked %u × %u = %u times.",
                next_run_params.num_groups,
                next_run_params.sample_size,
                next_run_params.num_units);
        ImGui::PopItemWidth();

        ImGui::Separator();

        TextIcon(ICON_LC_SPROUT); ImGui::SameLine(icon_width);

        ImGui::BeginDisabled(next_run_params.seed_from_time);
        if (next_run_params.seed_from_time) {
            next_run_params.seed = rand_get_seed_from_time();
        }
        f32 checkbox_size = ImGui::GetFrameHeight();
        ImGui::PushItemWidth(option_width - checkbox_size);
        ImGui::InputScalar(
                "##RNG seed", ImGuiDataType_U64, &next_run_params.seed, NULL, NULL, "%" PRIu64);
        ImGui::EndDisabled();

        ImGui::PushStyleVarX(ImGuiStyleVar_ItemSpacing, 0.0f);
        ImGui::SameLine();
        char const* rng_clock_icon = next_run_params.seed_from_time
            ? ICON_LC_ALARM_CLOCK "##SeedWithTime" : ICON_LC_ALARM_CLOCK_OFF "##SeedWithTime";
        if (ImGui::Button(rng_clock_icon,
                          ImVec2(checkbox_size, checkbox_size))) {
            next_run_params.seed_from_time = !next_run_params.seed_from_time;
        }
        ImGui::PopStyleVar();
        ImGui::SameLine();
        ImGui::Text("RNG seed");

        //TextIcon(ICON_LC_CLOCK); ImGui::SameLine(icon_width);
        //ImGui::Checkbox("Seed with current time", &next_run_params.seed_from_time);
        ImGui::PopItemWidth();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Target##Header", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Target");
        TextIcon(ICON_LC_CROSSHAIR); ImGui::SameLine(icon_width);
        ImGui::PushItemWidth(option_width);
        if (ImGui::BeginCombo("Target", targets[next_run_params.target_idx].name, 0)) {
            for (u32 i = 0; i < (u32)ARRAY_SIZE(targets); i++) {
                bool is_selected = (next_run_params.target_idx == i);
                if (ImGui::Selectable(targets[i].name, is_selected)) {
                    next_run_params.target_idx = i;
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        TextIconGhost(); ImGui::SameLine(icon_width);
        ImGui::TextUnformatted(targets[next_run_params.target_idx].description);
        ImGui::PopItemWidth();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Verifier##Header", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("Verifier");
        TextIcon(ICON_LC_LIST_CHECK); ImGui::SameLine(icon_width);
        ImGui::Checkbox("Verify correctness of target output", &next_run_params.verifier_enabled);
        if (next_run_params.verifier_enabled) {
            TextIconGhost(); ImGui::SameLine(icon_width);
            ImGui::PushItemWidth(option_width);
            if (ImGui::BeginCombo("Verifier", verifiers[next_run_params.verifier_idx].name, 0)) {
                for (u32 i = 0; i < (u32)ARRAY_SIZE(verifiers); i++) {
                    bool is_selected = (next_run_params.verifier_idx == i);
                    if (ImGui::Selectable(verifiers[i].name, is_selected)) {
                        next_run_params.verifier_idx = i;
                    }
                    if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            TextIconGhost(); ImGui::SameLine(icon_width);
            ImGui::TextUnformatted(verifiers[next_run_params.verifier_idx].description);
            ImGui::PopItemWidth();
        }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Profiler options##Header", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushItemWidth(ImGui::GetFontSize() * 3);

        TextIcon(ICON_LC_COFFEE); ImGui::SameLine(icon_width);
        ImGuiDragU32(
                "Warmup (ms)",
                &next_run_params.warmup_ms,
                10.0f, 0, U32_MAX, "%u",
                ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine(); HelpMarker(
                "Perform dummy computations to induce a transition to the boost frequency "
                "before commencing the workload."
                "\n\n"
                "Set this to zero if the processor doesn't support dynamic frequency scaling. ");

        TextIcon(ICON_LC_REPEAT); ImGui::SameLine(icon_width);
        ImGuiDragU32(
                "Repetitions",
                &next_run_params.repetitions,
                1, 1, U32_MAX, "%u",
                ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine(); HelpMarker(
                "Perform the entire test run multiple times, using the same inputs, storing "
                "only the minimum time measured for each test unit (i.e., for each input). "
                "This serves to discard faulty measurements due to thread and process pre-empting. "
                "Repetitions will be done serially: The entire run will be performed, and then "
                "the seed will be reset to its initial value and the run will start over. "
                "\n\n"
                "Increase this parameter if you need high-precision measurements but observe "
                "poor repeatability across identical test runs. Decrease this parameter if you "
                "are timing a slower algorithm and don't require good repeatability. "
                "\n\n"
                "Beware: If you are making very brief runs, repetitions will yield artificially "
                "low computation times. This is (presumably) because the CPU is caching the entire "
                "computation in its branch predictor. If you experience this problem, increase "
                "the sample size.");

        TextIconGhost(); ImGui::SameLine(icon_width);
        ImGui::Text(
                u8"The target will be invoked %u × %u = %" PRIu64 " times.",
                next_run_params.num_units,
                next_run_params.repetitions,
                // NOTE Things like this should be computed not here, but in a lower layer.
                (u64)next_run_params.num_units * (u64)next_run_params.repetitions);

        ImGui::PopItemWidth();
        ImGui::Separator();

        TextIconGhost(); ImGui::SameLine(icon_width);
        ImGui::Checkbox("Run in separate thread", &next_run_params.separate_thread);
        ImGui::SameLine(); HelpMarker(
                "Disabling this option will make the results more repeatable, but the GUI will "
                "stop responding until the profiler is finished.");
        ImGui::Separator();

        TextIcon(ICON_LC_TIMER); ImGui::SameLine(icon_width);
        ImGui::Text("Timing method:");
        ImGui::SameLine(); HelpMarker(
                "If in doubt, pick RDTSC, as it is highly precise and fairly reliable, and exists "
                "on all x86-64 CPUs."
                "\n\n"
                "On some newer AMD CPUs, RDPRU (not yet implemented) is more accurate than RDTSC."
                "\n\n"
                "Another acceptable choice (on Windows) is QPC. Note that there's no benefit to "
                "using QPC when RDTSC is available, because QPC uses the TSC internally but has "
                "a lower resolution. So QPC should only be used on older systems that lack a TSC."
                "\n\n"
                "Both QTCT and QPCT also often use RDTSC internally, but when the thread (resp. "
                "process) is pre-empted they compensate by subtracting. Note that QPCT gives the "
                "_sum_ of timings of all threads in the current process, including those unrelated "
                "to the profiler target, so its output data will be higher. These two methods "
                "may fail to convert to accurate wall time. "
            );
        for (u32 i = 0; i < TIMING_METHOD_ID_MAX; ++i) {
            if (timing_methods[i].available[host->os]) {
                TextIconGhost(); ImGui::SameLine(icon_width);
                if (ImGui::RadioButton(timing_methods[i].name_long,
                                       next_run_params.timing == (TimingMethodID)i)) {
                    next_run_params.timing = (TimingMethodID)i;
                }
            }
        }
        TextIconGhost(); ImGui::SameLine(icon_width);
        ImGui::Checkbox("Adjust for timer overhead", &next_run_params.adjust_for_timer_overhead);
        ImGui::SameLine(); HelpMarker(
                "Try to measure, and compensate for, the time required to execute the timing "
                "instructions.\n\n"
                "This is unreliable for certain systems and/or timing methods.");

        ImGui::Separator();

        TextIcon(ICON_LC_INFO); ImGui::SameLine(icon_width);
        ImGui::Text("Timer information:");
        TextIconGhost(); ImGui::SameLine(icon_width);
        if (ImGui::BeginTable("TimerInfo", 4, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TableSetColumnIndex(1); ImGui::Text("Period");
            ImGui::TableSetColumnIndex(2); ImGui::Text("Frequency");

            if (timing_methods[TIMING_RDTSC].available[host->os]) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("TSC:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f ns",
                            (host->tsc_frequency == 0) ? 0.0f : (1.e9f / host->tsc_frequency));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.0f MHz", host->tsc_frequency * 1e-6f);
                ImGui::TableSetColumnIndex(3); HelpMarker(
                        "Time Stamp Counter (TSC) units correspond (roughly speaking) to CPU clock "
                        "cycles. On very old CPUs, TSC units correspond exactly to CPU cycles, "
                        "whereas modern CPUs have an \"Invariant TSC\" that runs at a constant "
                        "frequency independent of dynamic frequency scaling and shared across "
                        "all cores. This frequency coincides with the base clock frequency on most, "
                        "but not all, CPUs. "
                        "\n\n"
                        "This is a measured estimate (as most CPUs do not report the TSC frequency).");
            }

            if (timing_methods[TIMING_QPC].available[host->os]) {
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
            }

            if (timing_methods[TIMING_CLOCK_GETTIME].available[host->os]) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("clock_gettime():");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%" PRIu64 " ns", host->clock_gettime_period);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.1f MHz", 1000.0f / host->clock_gettime_period);
                ImGui::TableSetColumnIndex(3); HelpMarker(
                        "clock_gettime() is a POSIX function that gives a high-resolution "
                        "timestamp. The period here is as reported by clock_getres(); it does "
                        "not necessarily coincide with the actual granularity of this timer.");
            }

            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Processor information##Header"
                                /*, ImGuiTreeNodeFlags_DefaultOpen*/ )) {
        TextIcon(ICON_LC_CPU); ImGui::SameLine(icon_width);
        if (ImGui::BeginTable("CPUInfo", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("Processor:");
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", host->cpu_name);
            ImGui::SameLine(); HelpMarker(
                    "More detailed information may be obtained through other utilities such as: "
                    "\n\n"
                    "Windows: CPU-Z; Sysinternals Coreinfo"
                    "\n"
                    "Linux: CPU-X"
                );

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

    }
    ImGui::EndChild();

    PushBigButton();
    bool go_requested = ImGui::Button("BEGIN  " ICON_LC_WIND);
    PopBigButton();
    if (go_requested) {
        if (!profiler_params_valid(next_run_params)) {
            logger_append(l, LOG_LEVEL_ERROR, "Cannot run profiler: Invalid parameters.");
        } else {
            // Allocate space for the new run's results.
            ProfilerResult result_tmp = profiler_result_create(next_run_params);
            if (!result_tmp.valid) {
                logger_append(l, LOG_LEVEL_ERROR,
                              "Failed to allocate memory for new profiler run.");
            } else {
                static u64 unique_run_id = 1;
                Profrun* run = darray_profrun_push(a, runs);
                run->id = unique_run_id++;
                run->state = PROFRUN_PENDING;
                run->sync = {0};
                run->thread_handle = 0;
                run->params = next_run_params;
                run->result = result_tmp;
                run->intent_visible = true;
                run->fresh = false;
                logger_appendf(l, LOG_LEVEL_DEBUG,
                               "(ID %" PRIu64 ") Queued profiler run.", run->id);
            }
        }
    }

    ImGui::End();  // Profiler window

    ImGui::Begin("Results List");
    static bool visible_display_options = true;
    ImGui::BeginChild("ProfilerResultsChild",
                      ImVec2(0, -(1 + (visible_display_options ? 6 : 0)) *
                             ImGui::GetFrameHeightWithSpacing()));
    {
        if (ImGui::BeginTable("ResultsList", 3, ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupScrollFreeze(0, 1);  // Top row always visible.
            ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthFixed);
            bool table_empty = runs->len == 0;

            // Table header

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // No header behind checkbox/icons, because that would be ugly.
            //ImGui::TableHeader("##Visible");
            //ImGui::SameLine(0,0);
            usize num_runs_intent_visible = 0;
            // Recomputing in every frame; yuck!
            for (usize i = 0; i < runs->len; ++i) {
                if (runs->data[i].intent_visible) {
                    ++num_runs_intent_visible;
                }
            }
            bool all_intent_visible = num_runs_intent_visible == runs->len;
            ImGui::BeginDisabled(table_empty);
            // Match the size/shape of the checkboxes below.
            f32 checkbox_size = ImGui::GetFrameHeight();
            if (ImGui::Button(ICON_LC_CHART_SPLINE "##AllResultsVisibility",
                              ImVec2(checkbox_size, checkbox_size))) {
                for (usize i = 0; i < runs->len; ++i) {
                    runs->data[i].intent_visible = !all_intent_visible;
                }
            }
            ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(1);

            // NOTE Aesthetically, it's not clear which element is best for the header. A button has
            // precisely the correct height to line up with the elements in the neighboring columns
            // (we push colors instead of disabling the button, because we don't want greyed-out
            // text). On the other hand, when the very first result has a colored background, a
            // button in the header looks weird. But TableSetBgColor(ImGuiTableBgTarget_CellBg, ...)
            // is also not ideal, because the cell is slightly taller than the buttons in the
            // adjacent column headers. Another, related problem: the TreeNode mouseover visual
            // effect is smaller than the ImGuiTableBgTarget_CellBg rectangle.
            //ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
            //                       ImGui::GetColorU32(ImGuiCol_Button));
            ImGui::Text("Result Details");
            /*ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_Button));
            if (ImGui::Button("Result Details##ResultDetailsHeader",
                              ImVec2(-1, 0))) {
                // We could sort `runs` here, perhaps.
            }
            ImGui::PopStyleColor(2);*/

            ImGui::TableSetColumnIndex(2);

            ImGui::BeginDisabled(table_empty);
            if (ImGui::Button(ICON_LC_TRASH_2)) {
                ImGui::OpenPopup("Delete all results");
            }
            ImGui::EndDisabled();
            if (ImGui::BeginPopup("Delete all results", 0)) {
                if (table_empty) {
                    // User already cleared the data in some other way.
                    ImGui::CloseCurrentPopup();
                }
                ImVec2 popup_button_size = ImVec2(ImGui::GetFontSize() * 3.5f, 0.f);
                ImGui::Text("Delete all results?");
                if (ImGui::Button("Confirm", popup_button_size)) {
                    logger_appendf(l, LOG_LEVEL_DEBUG,
                                   "User requested deletion of all %u profiler run%s.",
                                   runs->len,
                                   (runs->len == 1) ? "" : "s");
                    for (usize i = 0; i < runs->len; ++i) {
                        if (profrun_try_delete(l, runs, i)) {
                            --i;
                        }
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", popup_button_size)) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // Table contents

            bool delete_requested = false;
            usize delete_idx = 0;
            for (usize i = 0; i < runs->len; ++i) {
                Profrun* run = &(runs->data[i]);
                ProfilerResult* result = &run->result;
                ProfilerParams* p = &run->params;

                // The ImGui ID must be tied to the actual result, because the runs may get
                // deleted and/or reordered, and we want the GUI status (e.g., which treenodes
                // are open) to persist.
                ImGui::PushID((i32)(runs->data[i].id));
                char const* result_name = targets[p->target_idx].name;
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::Checkbox("", &(runs->data[i].intent_visible));

                ImGui::TableSetColumnIndex(1);
                ImU32 cell_bg_color = {0};
                switch (run->state) {
                case PROFRUN_PENDING: {
                    cell_bg_color = (ImU32)0x4000FFFF;
                } break;
                case PROFRUN_RUNNING: {
                    cell_bg_color = (ImU32)0x40FF8000;
                } break;
                case PROFRUN_ABORT_REQD:
                case PROFRUN_ABORTING: {
                    cell_bg_color = (ImU32)0x40CC00FF;
                } break;
                case PROFRUN_DONE_SUCCESS: {
                    if (!(p->verifier_enabled) ||
                        *(result->verification_accept_count) == p->num_units) {
                        cell_bg_color = (ImU32)ImGuiCol_Header;
                    } else {
                        // Verifier failed to accept all units.
                        cell_bg_color = (ImU32)0x400000FF;
                    }
                } break;
                case PROFRUN_DONE_FAILURE: {
                    cell_bg_color = (ImU32)0x40CC00FF;
                } break;
                default:
                case PROFRUN_STATE_MAX: {
                    cell_bg_color = (ImU32)0xFF808080;
                } break;
                }  // switch (run->state)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, cell_bg_color);

                bool tree_node_open = ImGui::TreeNodeEx(
                        result_name,
                        ImGuiTreeNodeFlags_SpanAllColumns |
                        ImGuiTreeNodeFlags_SpanAvailWidth |
                        ImGuiTreeNodeFlags_AllowOverlap);
                if (profrun_busy(run)) {
                    // Show progress. NOTE This is a memory-unsafe read (may be torn), but we'll
                    // leve it as is because the multithreading is going to be torn out anyway (to
                    // be replaced by multiple processes).
                    ImGui::SameLine();
                    // Default color for progress bar is ugly orange.
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, (ImU32)0x8000FF00);
                    ImGui::ProgressBar(*(result->progress), ImVec2(-1.0f, 0.0f));
                    ImGui::PopStyleColor();
                }

                if (tree_node_open) {
                    if (ImGui::Button(ICON_LC_COPY " Again")) {
                        next_run_params = *p;
                    }
                    ImGui::SameLine();
                    HelpMarker("Re-load these parameters to use for the next run.");
                    ImGui::Text("Sampler: %s", samplers[p->sampler_idx].name);
                    ImGui::Text("Range: (%u, %u, %u)", p->ns.lower, p->ns.stride, p->ns.upper);

                    ImGui::Text("Sample size: %u", p->sample_size);
                    ImGui::Text("Total units: %u", p->num_units);
                    ImGui::Text("Seed: %" PRIu64, p->seed);
                    /*  // Copy to clipboard -- works, but is very ugly.
                    ImGui::SameLine();
                    if (ImGui::Button(ICON_LC_CLIPBOARD)) {
                        #define MAX_SEED_STR_LEN 21
                        char seed_str[MAX_SEED_STR_LEN] = {0};
                        snprintf(seed_str, 21, PRIu64, p->seed);
                        ImGui::SetClipboardText(seed_str);
                        #undef MAX_SEED_STR_LEN
                    }
                    */
                    ImGui::Text("Timing: %s", timing_methods[p->timing].name_short);
                    ImGui::Text("Repetitions: %u", p->repetitions);
                    ImGui::Text("Verification: %s",
                                p->verifier_enabled
                                ? (profrun_done(run)  // Avoid race condition.
                                   ? (p->num_units == *(result->verification_accept_count)
                                      ? ICON_LC_CHECK " Success"
                                      : ICON_LC_X " Failure")
                                   : "Pending")
                                : "Off");
                    ImGui::TreePop();
                }

                ImGui::TableSetColumnIndex(2);
                if (ImGui::Button(ICON_LC_X)) {
                    // Queue deletion for later, so we don't invalidate the loop index.
                    delete_requested = true;
                    delete_idx = i;
                }
                ImGui::PopID();
            }  // for each run
            if (delete_requested) {
                profrun_try_delete(l, runs, delete_idx);
            }
            // Autoscroll when user adds new entries, but not when use opens treenodes.
            static usize prev_table_len = 0;
            if ((runs->len > prev_table_len) &&
                (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())) {
                ImGui::SetScrollHereY(1.0f);
            }
            prev_table_len = runs->len;
            ImGui::EndTable();
        }  // table
    }
    ImGui::EndChild();

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
        ImGui::Checkbox("Live view", &guiconf->live_view);
        ImGui::SameLine();
        HelpMarker("Watch the results as they come in. This may decrease performance, and is "
                   "not memory safe.\n\nNot recommended.");

        bool visible_any_now =
            guiconf->visible_data_individual ||
            guiconf->visible_data_mean ||
            guiconf->visible_data_median ||
            guiconf->visible_data_bounds;
        if (!visible_any_prev && visible_any_now && guiconf->auto_zoom) {
            // Bugfix: If user made new data while nothing was visible, we must re-adjust axes.
            for (usize i = 0; i < runs->len; ++i) {
                if (profrun_actually_visible(&(runs->data[i]), guiconf->live_view)) {
                    ImPlot::SetNextAxesToFit();
                    break;
                }
            }
        }
    }

    ImGui::End();  // Window: Profiler Runs


    ImGui::Begin("Running Time"/*, visible*/);

    ImGui::BeginChild("PlotChild", ImVec2(0, -1),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                      ImGuiWindowFlags_None);

    for (usize i = 0; i < runs->len; ++i) {
        Profrun* run = &runs->data[i];
        if (run->fresh) {
            if (profrun_actually_visible(run, guiconf->live_view) && guiconf->auto_zoom) {
                // Set plot axes to fit the bounds of the new data.
                ImPlot::SetNextAxesToFit();
            }
            run->fresh = false;
        }
    }


    if (ImPlot::BeginPlot(
                "Running Time",
                ImVec2(-1, -1),
                ImPlotFlags_NoTitle |
                ImPlotFlags_NoMenus |
                ImPlotFlags_NoBoxSelect
                /* | ImPlotFlags_Crosshairs */  // Crosshairs are laggy on Linux/Wine.
            )) {

        char const* time_axis_label = "Time (ns)";
        ImPlot::SetupAxes("n", time_axis_label, 0, 0);
        ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_NoButtons);

        for (usize i = 0; i < runs->len; ++i) {
            Profrun* run = &(runs->data[i]);
            ProfilerParams* params = &run->params;
            ProfilerResult* result = &run->result;
            if (!profrun_actually_visible(run, guiconf->live_view)) {
                // If we keep going here, the user will see a "live" view of the results as
                // the profiler thread writes them; however, this is not memory-safe.
                continue;
            }

            char const* plot_name = targets[params->target_idx].name;

            if (guiconf->visible_data_bounds) {
                ImPlot::PushStyleVar(ImPlotStyleVar_FillAlpha, 0.25f);
                ImPlot::PlotShaded(
                        plot_name,
                        &result->groups[0].n,
                        &result->groups[0].time_min,
                        &result->groups[0].time_max,
                        // NOTE ImPlot requires i32; this is possibly a bug for us, but
                        // we won't be using ImPlot forever so we won't bother fixing this.
                        (i32)params->num_groups,
                        0,
                        0,
                        sizeof(*result->groups));
                ImPlot::PopStyleVar();
            }

            if (guiconf->visible_data_median) {
                ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, 2.0f);
                ImPlot::PlotLine(
                        plot_name,
                        &result->groups[0].n,
                        &result->groups[0].time_median,
                        (i32)params->num_groups,
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
                        (i32)params->num_groups,
                        0,
                        0,
                        sizeof(*result->groups));
            }

            if (guiconf->visible_data_individual ||
                (guiconf->live_view && profrun_busy(run))) {
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
                        (i32)params->num_units,
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
    #ifdef _WIN32
    SetProcessDPIAware();
    #endif

    // Set up SDL.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
    {
        printf("Error: %s\n", SDL_GetError());
        return -1;
    }

    // Select GL+GLSL version.
#if defined(IMGUI_IMPL_OPENGL_ES2)
    // GL ES 2.0 + GLSL 100 (WebGL 1.0)
    const char* glsl_version = "#version 100";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#elif defined(IMGUI_IMPL_OPENGL_ES3)
    // GL ES 3.0 + GLSL 300 es (WebGL 2.0)
    const char* glsl_version = "#version 300 es";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#elif defined(__APPLE__)
    // GL 3.2 Core + GLSL 150
    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // Always required on Mac
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    // From 2.0.18: Enable native IME.
#ifdef SDL_HINT_IME_SHOW_UI
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif

    // Create window with graphics context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_WindowFlags window_flags = (SDL_WindowFlags)(
            SDL_WINDOW_OPENGL |
            SDL_WINDOW_RESIZABLE |
            SDL_WINDOW_MAXIMIZED |
            SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* window = SDL_CreateWindow(
            "Sabrewing",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            1280,
            720,
            window_flags);
    if (window == nullptr)
    {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return -1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (gl_context == nullptr)
    {
        printf("Error: SDL_GL_CreateContext(): %s\n", SDL_GetError());
        return -1;
    }

    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);  // Enable vsync

    // We care most about the profiler thread; the GUI thread should not interfere.
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);

    SDL_DisplayMode display_mode = {0};
    SDL_GetCurrentDisplayMode(0, &display_mode);

    // Set up Dear ImGui context.
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
    io.IniFilename = "sabrewing.ini";

    // Set up platform/renderer backends.
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Initialize user-facing GUI options.
    GuiConfig guiconf = {0};
    guiconf.visible_imgui_demo_window = false;
    guiconf.visible_implot_demo_window = false;
    guiconf.visible_imgui_metrics_window = false;
    guiconf.visible_log_window = true;
    //guiconf.visible_profiler_window = true;
    guiconf.visible_data_individual = false;
    guiconf.visible_data_mean = true;
    guiconf.visible_data_median = false;
    guiconf.visible_data_bounds = true;
    guiconf.log_show_timestamps = true;
    guiconf.auto_zoom = true;
    guiconf.live_view = false;

    // GUI styling/theme
    GuiStyle guistyle;
    guistyle.is_dark = false;
    guistyle.font_size_min = 8;
    guistyle.font_size_max = 60;
    // Adjust default font size based on screen resolution.
    guistyle.font_size = CLAMP(
            (u8)(0.5 + display_mode.w / 100),
            guistyle.font_size_min,
            guistyle.font_size_max);
    guistyle.font_size_intent = guistyle.font_size;
    bool guistyle_changed = true;

    // Global state (non-GUI)
    Logger global_log = logger_create();
    Arena global_arena = arena_create(GLOBAL_ARENA_SIZE);
    HostInfo host = {0};
    darray_profrun profiler_runs = darray_profrun_new(&global_arena, 5);

    // Main loop
    bool done = false;
    while (!done) {
        query_host_info(&host);  // Inside loop, to update timer data.

        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui
        // wants to use your inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main
        //   application, or clear/overwrite your copy of the mouse data.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main
        //   application, or clear/overwrite your copy of the keyboard data.
        // Generally you may always pass all inputs to dear imgui, and hide them from your
        // application based on those two flags.
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                done = true;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window))
                done = true;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        // Update styles. Note that the font must be set before ImGui::NewFrame(). (??)
        if (guistyle_changed) {
            set_imgui_style(&global_log, &io, guistyle.is_dark, guistyle.font_size);
            guistyle_changed = false;
        }

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
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
                //ImGui::MenuItem("Profiler Window", NULL, &guiconf.visible_profiler_window);
                ImGui::MenuItem("Log window", NULL, &guiconf.visible_log_window);
                if (ImGui::BeginMenu("Debug")) {
                    ImGui::MenuItem("ImGui demo window", NULL,
                                    &guiconf.visible_imgui_demo_window);
                    ImGui::MenuItem("ImPlot demo window", NULL,
                                    &guiconf.visible_implot_demo_window);
                    ImGui::MenuItem("ImGui metrics window", NULL,
                                    &guiconf.visible_imgui_metrics_window);
                    ImGui::EndMenu();
                }
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
        ImGui::Begin("(Root Dockspace)", nullptr, dockspace_flags);
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

        // Computation
        manage_profiler_workers(&global_log, &host, &profiler_runs);

        // Our windows
        show_log_window(&guiconf, &global_log);
        show_profiler_windows(&guiconf, &global_log, &global_arena, &host, &profiler_runs);

        // Rendering
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
        glClearColor(
                clear_color.x * clear_color.w,
                clear_color.y * clear_color.w,
                clear_color.z * clear_color.w,
                clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Update and Render additional platform windows.
        // (Platform functions may change the current OpenGL context, so we save/restore it. If they
        // don't change the context, we could call SDL_GL_MakeCurrent(window, gl_context) directly.)
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow();
            SDL_GLContext backup_current_context = SDL_GL_GetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            SDL_GL_MakeCurrent(backup_current_window, backup_current_context);
        }

        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
