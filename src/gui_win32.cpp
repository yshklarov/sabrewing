#include "imgui.h"
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

#include <intrin.h>
#include <stdint.h>

#include "Lucide_Symbols.h"

#include "util.c"
#include "logger.c"
#include "cpuinfo.c"
#include "sort.c"
#include "profiler.c"


/**** Constants ****/

#define GLOBAL_ARENA_SIZE 1024*1024*10


/**** Types ****/

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


/****  Functions ****/

void set_imgui_style(logger* l, ImGuiIO* io, bool is_dark, u8 font_size)
{
    // TODO Note ImGui has an experimental feature, branch feature/dynamic_fonts, as of 2025-03-05,
    //      for better font resizing. We'll switch to it once it's ready.

    // TODO load multiple fonts (for code editing) and use ImGui::PushFont()/PopFont() to select
    //      them (see FONTS.md). AddFontFromFileTTF() returns ImFont*; store it so we can select it
    //      later.

    // TODO Perhaps '#define IMGUI_ENABLE_FREETYPE' in imconfig.h to use Freetype for higher quality
    //      font rendering.

    // TODO Detect DPI changes and set style/scaling appropriately. See:
    // https://github.com/ocornut/imgui/blob/master/
    //                 docs/FAQ.md#q-how-should-i-handle-dpi-in-my-application

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
                                1.2f * ImGui::GetFrameHeight(),
                                1.0f * ImGui::GetFrameHeight()));
}
static void PopBigButton() { ImGui::PopStyleVar(); }
static f32 GetBigButtonHeightWithSpacing()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(
                                1.2f * ImGui::GetFrameHeight(),
                                1.0f * ImGui::GetFrameHeight()));
    f32 result = ImGui::GetFrameHeightWithSpacing();
    ImGui::PopStyleVar();
    return result;
}

/*bool RightAlignedButton(char const * label) {
    f32 width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
    return ImGui::Button(label);
}*/

void TextIcon(char* icon) {
    f32 length = ImGui::GetFrameHeight();  // Always re-fetch, in case user changed it.
    ImGui::BeginChildFrame(ImGui::GetID(icon),
                      {length, length},
                      ImGuiWindowFlags_NoBackground |
                      ImGuiWindowFlags_NoDecoration |
                      ImGuiWindowFlags_NoNav);
    ImGui::Text(icon);
    ImGui::EndChild();
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

    if (ImGui::Button(ICON_LC_ERASER " Clear log")) {
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

    // TODO Add log search/filter textbox.

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
        host_info* host,
        darray_profiler_result* results)
{
    ImGui::Begin("Profiler" /*, visible*/);

    static profiler_params next_run_params = profiler_params_default();

    f32 icon_width = ImGui::GetFrameHeightWithSpacing();

    ImGui::BeginChild("ProfilerParamsConfigurationChild",
                      ImVec2(0, -GetBigButtonHeightWithSpacing()));
    {

    if (ImGui::CollapsingHeader("Sampler##Header", ImGuiTreeNodeFlags_DefaultOpen)) {
        TextIcon(ICON_LC_DICES); ImGui::SameLine(icon_width);
        ImGui::PushItemWidth(ImGui::GetFontSize() * 12 - icon_width);
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
        ImGui::PopItemWidth();
        ImGui::Separator();

        ImGui::PushItemWidth(ImGui::GetFontSize() * 12);

        // TODO Implement our own custom ImGui::DragIntRange2, which supports u64 and a third
        //      "stride" parameter in the middle, and does better bounds checking; then, change
        //      profiler_params to use a range_u32 or range_u64. Do the same for sample_size.
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
        ImGui::PopItemWidth();

        ImGui::Separator();

        TextIcon(ICON_LC_SPROUT); ImGui::SameLine(icon_width);

        ImGui::BeginDisabled(next_run_params.seed_from_time);
        if (next_run_params.seed_from_time) {
            next_run_params.seed = rand_get_seed_from_time();
        }
        ImGui::PushItemWidth(ImGui::GetFontSize() * 12 - icon_width);
        ImGui::InputScalar(
                "RNG seed", ImGuiDataType_U64, &next_run_params.seed, NULL, NULL, "%llu");
        ImGui::EndDisabled();
        ImGui::Checkbox("Seed with current time", &next_run_params.seed_from_time);
        ImGui::PopItemWidth();
    }

    if (ImGui::CollapsingHeader("Target##Header", ImGuiTreeNodeFlags_DefaultOpen)) {

        TextIcon(ICON_LC_CROSSHAIR); ImGui::SameLine(icon_width);
        ImGui::PushItemWidth(ImGui::GetFontSize() * 12 - icon_width);
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

    if (ImGui::CollapsingHeader("Verifier##Header", ImGuiTreeNodeFlags_DefaultOpen)) {
        TextIcon(ICON_LC_LIST_CHECK); ImGui::SameLine(icon_width);
        ImGui::PushItemWidth(ImGui::GetFontSize() * 12 - icon_width);
        ImGui::Checkbox("Verify correctness of target output", &next_run_params.verify_correctness);
        ImGui::SameLine(); HelpMarker(
                "Verification involves a simple checksum, and may (occasionally) give false "
                "positives.");
    }

    if (ImGui::CollapsingHeader("Profiler options##Header", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushItemWidth(ImGui::GetFontSize() * 3);

        TextIcon(ICON_LC_COFFEE); ImGui::SameLine(icon_width);
        ImGui::DragInt(
                "Warmup (ms)",
                &next_run_params.warmup_ms,
                1.0f, 0, I32_MAX, "%d",
                ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine(); HelpMarker(
                "Perform sham computations to induce a transition to the boost frequency "
                "before commencing the workload."
                "\n\n"
                "Set this to zero if the processor doesn't support dynamic frequency scaling. ");

        TextIcon(ICON_LC_REPEAT); ImGui::SameLine(icon_width);
        ImGui::DragInt(
                "Repetitions",
                &next_run_params.repetitions,
                1, 1, I32_MAX, "%d",
                ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine(); HelpMarker(
                "Perform the entire test run multiple times, using the same inputs, storing "
                "only the minimum time measured for each test unit (i.e., for each input). "
                "This serves to discard faulty measurements due to thread and process pre-empting."
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

        ImGui::PopItemWidth();
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
        for (i32 i = 0; i < TIMING_METHOD_ID_MAX; ++i) {
            if (timing_methods[i].available_win32) {
                if (ImGui::RadioButton(timing_methods[i].name_long,
                                       next_run_params.timing == (timing_method_id)i)) {
                    next_run_params.timing = (timing_method_id)i;
                }
            }
        }
        ImGui::Checkbox("Adjust for timer overhead", &next_run_params.adjust_for_timer_overhead);
        ImGui::SameLine(); HelpMarker(
                "Try to measure, and compensate for, the time required to execute the timing "
                "instructions.\n\n"
                "This is unreliable for certain systems and/or timing methods.");

        ImGui::Separator();

        TextIcon(ICON_LC_INFO); ImGui::SameLine(icon_width);
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
            ImGui::Text("%.3f ns", (host->tsc_frequency == 0) ? 0.0f : (1.e9f / host->tsc_frequency));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.0f MHz", host->tsc_frequency * 1e-6f);
            ImGui::TableSetColumnIndex(3); HelpMarker(
                "Time Stamp Counter (TSC) units correspond (roughly speaking) to CPU clock cycles. "
                "On very old CPUs, TSC units correspond exactly "
                "to CPU cycles, whereas modern CPUs have an \"Invariant TSC\" that runs at a "
                "constant frequency independent of dynamic frequency scaling and shared across "
                "all cores. This frequency coincides with the base clock frequency on most, but "
                "not all, CPUs. "
                "\n\n"
                "This is a measured estimate (as most CPUs do not report the TSC frequency).");

            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Processor information##Header", ImGuiTreeNodeFlags_DefaultOpen)) {
        //ImGui::Text(ICON_LC_CPU);
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
        profiler_result result = profiler_execute(l, next_run_params, host);
        if (!result.id){
            logger_append(l, LOG_LEVEL_ERROR, "Profiler failed to run.");
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

    ImGui::Begin("Results List");
    static bool visible_display_options = true;
    // TODO Ewww! Do layout calculations better.
    ImGui::BeginChild("ProfilerResultsChild",
                      ImVec2(0, -(1 + (visible_display_options ? 5 : 0)) *
                             ImGui::GetFrameHeightWithSpacing()));
    {
        if (ImGui::BeginTable("ResultsList", 3, ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupScrollFreeze(0, 1);  // Top row always visible.
            ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthFixed);
            // TODO Drag & drop results to reorder; support multi-select drag & drop.
            // TODO Magnifier in results list: Auto-zoom to a single results item, and to all.

            // Table header

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // No header behind checkbox/icons, because that would be ugly.
            //ImGui::TableHeader("##Visible");
            //ImGui::SameLine(0,0);
            usize num_results_visible = 0;
            // Recomputing in every frame; yuck!
            for (usize i = 0; i < results->len; ++i) {
                if (results->data[i].gui.plot_visible) {
                    ++num_results_visible;
                }
            }
            bool all_visible = (num_results_visible == results->len) && (results->len > 0);
            // TODO Find, or build, a three-way checkbox with a "partial" setting.
            ImGui::BeginDisabled(results->len == 0);
            // Match the size/shape of the checkboxes below.
            f32 checkbox_size = ImGui::GetFrameHeight();
            if (ImGui::Button(ICON_LC_CHART_SPLINE "##AllResultsVisibility",
                              ImVec2(checkbox_size, checkbox_size))) {
                for (usize i = 0; i < results->len; ++i) {
                    results->data[i].gui.plot_visible = !all_visible;
                }
            }
            ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(1);
            // The button is the simplest element with the precisely correct dimensions and
            // background color. We push colors instead of disabling the button, because we don't
            // want greyed-out text.
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_Button));
            if (ImGui::Button("Result Details##ResultDetailsHeader",
                              ImVec2(-1, 0))) {
                // We could sort the results list here, perhaps.
            }
            ImGui::PopStyleColor(2);

            ImGui::TableSetColumnIndex(2);
            //ImGui::TableHeader("##Delete");
            //ImGui::SameLine(0,0);
            ImGui::BeginDisabled(results->len == 0);
            if (ImGui::Button(ICON_LC_TRASH_2)) {
                ImGui::OpenPopup("Delete all results");
            }
            ImGui::EndDisabled();
            if (ImGui::BeginPopup("Delete all results", NULL)) {
                if (results->len == 0) {
                    // User already cleared the data in some other way.
                    ImGui::CloseCurrentPopup();
                }
                ImVec2 popup_button_size = ImVec2(ImGui::GetFontSize() * 3.5f, 0.f);
                ImGui::Text("Delete all results?");
                if (ImGui::Button("Confirm", popup_button_size)) {
                    logger_appendf(l, LOG_LEVEL_DEBUG, "Destroying %d %s.", results->len,
                                   (results->len == 1) ? "result" : "results");
                    for (usize i = 0; i < results->len; ++i) {
                        result_destroy(&(results->data[i]));
                    }
                    darray_profiler_result_clear(results);
                    //requested_clear_all = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", popup_button_size)) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // Table contents

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
                ImGui::Checkbox("", &result->gui.plot_visible);
                //ImGui::SameLine();
                ImGui::TableSetColumnIndex(1);
                if (result->verification_reject_count > 0) {
                    ImU32 badness_color = (ImU32)(0x600000FF);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, badness_color);
                }
                if (ImGui::TreeNodeEx(result_name,
                                      ImGuiTreeNodeFlags_SpanAllColumns |
                                      ImGuiTreeNodeFlags_SpanAvailWidth |
                                      ImGuiTreeNodeFlags_AllowOverlap
                        )) {
                    profiler_params* p = &result->params;
                    if (ImGui::Button(ICON_LC_COPY " Again")) {
                        next_run_params = *p;
                    }
                    ImGui::SameLine();
                    HelpMarker("Re-load these parameters to use for the next run.");
                    ImGui::Text("Sampler: %s", samplers[p->sampler_idx].name);
                    ImGui::Text("Range: (%d, %d, %d)", p->ns.lower, p->ns.stride, p->ns.upper);
                    ImGui::Text("Sample size: %d", p->sample_size);
                    ImGui::Text("Total units: %d", result->len_units);
                    ImGui::Text("Seed: %llu", p->seed);
                    /*  // Copy to clipboard -- works, but is very ugly.
                    ImGui::SameLine();
                    if (ImGui::Button(ICON_LC_CLIPBOARD)) {
                        #define MAX_SEED_STR_LEN 21
                        char seed_str[MAX_SEED_STR_LEN] = {0};
                        snprintf(seed_str, 21, "%llu", p->seed);
                        ImGui::SetClipboardText(seed_str);
                        #undef MAX_SEED_STR_LEN
                    }
                    */
                    ImGui::Text("Timing: %s", timing_methods[p->timing].name_short);
                    ImGui::Text("Repetitions: %d", p->repetitions);
                    ImGui::Text("Verification: %s", p->verify_correctness
                                ? (0 == result->verification_reject_count
                                   ? ICON_LC_CHECK " Success"
                                   : ICON_LC_X " Failure")
                                : "Off");
                    // TODO Display more details:
                    //    - Total memory used by this result (i.e., size of local_arena)
                    //    - Timestamp: Began, finished
                    //    - Total time elapsed
                    //    - Profiling progress bar, with "pause"/"continue" button; hide results by
                    //      default while profiling, but allow user to manually select checkbox to
                    //      view live results in graph as they arrive.
                    ImGui::TreePop();
                }
                ImGui::TableSetColumnIndex(2);
                // TODO Table column with zoom magnifier. See: Icon fonts (ImGui: FONTS.md);
                //      Zooming to fit will require storing (double min/max for each axis)
                //      in the struct profiler_result.
                if (ImGui::Button(ICON_LC_X)) {
                    // TODO Prompt for confirmation, maybe?
                    // Queue deletion for later, so we don't invalidate the loop index.
                    delete_requested = true;
                    delete_idx = i;
                }
                ImGui::PopID();
            }  // for each result
            if (delete_requested) {
                logger_appendf(l, LOG_LEVEL_DEBUG,
                               "Destroying result ID %d.", results->data[delete_idx].id);
                result_destroy(&(results->data[delete_idx]));
                darray_profiler_result_remove(results, delete_idx);
            }
            ImGui::EndTable();
        }  // table
    }
    ImGui::EndChild();

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

    ImGui::BeginChild("PlotChild", ImVec2(0, -1),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                      ImGuiWindowFlags_None);
    if (ImPlot::BeginPlot(
                "Running Time",
                ImVec2(-1, -1),
                ImPlotFlags_NoTitle |
                ImPlotFlags_NoMenus |
                ImPlotFlags_NoBoxSelect
                /* | ImPlotFlags_Crosshairs */  // Crosshairs are laggy on Linux/Wine.
            )) {

        // TODO Allow user to select between nanoseconds and TSC units -- if we can come up with
        //      a reasonable thing to do when two plots are shown together with different units.
        char const* time_axis_label = "Time (ns)";
        ImPlot::SetupAxes("n", time_axis_label, 0, 0);
        ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_NoButtons);

        for (usize i = 0; i < results->len; ++i) {
            profiler_result* result = &(results->data[i]);
            if (!result->gui.plot_visible) {
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
                // TODO This gets slow when there are more than 50-100,000 points. Either (easiest)
                // resample (only plot a subset of points), or (better!) implement our own
                // PlotScatter that doesn't use ImDrawList but instead renders directly using
                // graphics shaders; this will also let us use custom/mixed types (u64 for n and
                // float for vertical axis).

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
    // TODO Restore maximized/restored state from before, just like remedybg. In fact, restore all
    //      GUI settings.
    //      See: https://learn.microsoft.com/en-us/windows/
    //         win32/api/winuser/nf-winuser-showwindow?redirectedfrom=MSDN

    #ifdef _WIN32
    ::SetProcessDPIAware();
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

    // GUI styling/theme
    gui_style guistyle;
    guistyle.is_dark = false;
    guistyle.font_size = 28;
    guistyle.font_size_intent = guistyle.font_size;
    guistyle.font_size_min = 8;
    guistyle.font_size_max = 60;
    bool guistyle_changed = true;

    // Global state (non-GUI)
    logger global_log = logger_create();
    arena global_arena = arena_create(GLOBAL_ARENA_SIZE);
    host_info host = {0};
    darray_profiler_result profiler_results = darray_profiler_result_new(&global_arena, 5);

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
        // TODO Detect DPI changes and set style/scaling appropriately. See:
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

        // Our windows
        show_log_window(&guiconf, &global_log);
        show_profiler_windows(&guiconf, &global_log, &global_arena, &host, &profiler_results);

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
