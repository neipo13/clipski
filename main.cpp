#define SDL_MAIN_HANDLED
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <SDL.h>
#include <SDL_opengl.h>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

// ============================================================================
// FFmpeg Encoding Presets (Matching your C:\Scripts\ffmpeg Discord scripts)
// Available placeholders: {start}, {end}, {input}, {output}
// ============================================================================
static const std::string FFMPEG_ARGS_DISCORD_1080P =
    "-y -ss {start} -to {end} -i \"{input}\" -vf \"scale=-2:1080,fps=60\" -crf 23 -preset slower -c:v libx264 -c:a aac -b:a 128k \"{output}\"";

static const std::string FFMPEG_ARGS_DISCORD_720P =
    "-y -ss {start} -to {end} -i \"{input}\" -vf \"scale=-2:720,fps=60\" -crf 32 -preset slower -c:v libx264 -c:a aac -b:a 128k \"{output}\"";

static const std::string FFMPEG_ARGS_LOSSLESS_COPY =
    "-y -ss {start} -to {end} -i \"{input}\" -c copy \"{output}\"";

enum class ExportPreset {
    Discord1080p,
    Discord720p,
    StreamCopy
};

// ============================================================================
// Global / App State
// ============================================================================
static Uint32 g_wakeup_on_mpv_render_update = 0;
static Uint32 g_wakeup_on_mpv_events        = 0;
static Uint32 g_wakeup_on_export_done       = 0;

struct RoutingConfig {
    std::string input_root  = "C:\\Users\\Andy\\Videos\\Radeon ReLive";
    std::string output_root = "D:\\Clips";
    // Lowercase source folder name -> target folder name
    std::unordered_map<std::string, std::string> aliases;
};

struct AppState {
    mpv_handle*         mpv    = nullptr;
    mpv_render_context* mpv_gl = nullptr;

    RoutingConfig config;

    std::string current_file;
    std::string target_output_dir_display;
    bool        file_loaded = false;
    bool        paused      = false;
    double      time_pos    = 0.0;
    double      duration    = 0.0;

    float trim_start = 0.0f;
    float trim_end   = 0.0f;
    bool  loop_clip  = false; // When true, loops strictly between trim_start and trim_end

    std::atomic<bool> is_exporting{false};
    std::atomic<int>  export_Result{0}; // 0 = none, 1 = success, -1 = error
    std::string       export_status_msg;
};

// ============================================================================
// String & Process Helpers (UTF-8 <-> UTF-16 for Windows APIs)
// ============================================================================
static std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) return {};
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), wstr.data(), size_needed);
    return wstr;
}

static std::string wide_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), str.data(), size_needed, nullptr, nullptr);
    return str;
}

static std::string to_lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static std::string trim_whitespace(const std::string& s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

static void replace_all(std::string& str, const std::string& from, const std::string& to) {
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

static std::string format_time(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    int total_ms = static_cast<int>(seconds * 1000.0 + 0.5);
    int mins     = (total_ms / 1000) / 60;
    int secs     = (total_ms / 1000) % 60;
    int ms       = (total_ms % 1000) / 10;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d.%02d", mins, secs, ms);
    return std::string(buf);
}

static std::string format_seconds_Arg(float val) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", val);
    return std::string(buf);
}

// ============================================================================
// Configuration & Output Directory Resolution
// ============================================================================
static fs::path get_exe_directory() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0) return fs::current_path();
    return fs::path(buf).parent_path();
}

static void load_routing_config(RoutingConfig& cfg) {
    // Built-in defaults so it works even if clipski.ini is deleted
    cfg.aliases["rocket league"]               = "Rockets";
    cfg.aliases["baldur's gate 3"]             = "bg3";
    cfg.aliases["ea sports college football 27"] = "cfb";
    cfg.aliases["peak"]                        = "peak";

    fs::path ini_path = get_exe_directory() / "clipski.ini";
    if (!fs::exists(ini_path)) {
        // Also check parent / source directory if running from build/Release
        fs::path alt_path = get_exe_directory().parent_path().parent_path() / "clipski.ini";
        if (fs::exists(alt_path)) {
            ini_path = alt_path;
        } else {
            return;
        }
    }

    std::ifstream file(ini_path);
    if (!file.is_open()) return;

    std::string section;
    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = trim_whitespace(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            section = to_lower_ascii(trim_whitespace(trimmed.substr(1, trimmed.size() - 2)));
            continue;
        }

        size_t eq_pos = trimmed.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = trim_whitespace(trimmed.substr(0, eq_pos));
        std::string val = trim_whitespace(trimmed.substr(eq_pos + 1));
        if (key.empty() || val.empty()) continue;

        if (section == "paths") {
            std::string lkey = to_lower_ascii(key);
            if (lkey == "input_root") {
                cfg.input_root = val;
            } else if (lkey == "output_root") {
                cfg.output_root = val;
            }
        } else if (section == "aliases") {
            cfg.aliases[to_lower_ascii(key)] = val;
        }
    }
}

// Determines where exported clips for `input_filepath` should be saved.
// If `input_filepath` is inside `config.input_root\<GameFolder>\...`, routes to `config.output_root\<MappedGameFolder>\`.
// Otherwise, saves in the same folder as `input_filepath`.
static fs::path resolve_output_directory(const RoutingConfig& cfg, const fs::path& in_path) {
    fs::path default_dir = in_path.parent_path();
    if (cfg.input_root.empty() || cfg.output_root.empty()) {
        return default_dir;
    }

    std::error_code ec;
    fs::path norm_in   = fs::weakly_canonical(in_path, ec);
    fs::path norm_root = fs::weakly_canonical(fs::u8path(cfg.input_root), ec);

    std::string in_str   = to_lower_ascii(norm_in.u8string());
    std::string root_str = to_lower_ascii(norm_root.u8string());

    if (!root_str.empty() && (root_str.back() == '\\' || root_str.back() == '/')) {
        root_str.pop_back();
    }

    // Check if norm_in starts with norm_root + separator
    if (in_str.size() > root_str.size() &&
        in_str.compare(0, root_str.size(), root_str) == 0 &&
        (in_str[root_str.size()] == '\\' || in_str[root_str.size()] == '/')) {

        // Extract relative path inside input_root
        fs::path rel = norm_in.lexically_relative(norm_root);
        auto it = rel.begin();
        if (it != rel.end()) {
            // Check if the file is inside a game subfolder (i.e., rel has at least 2 components: <GameFolder>/<file.mp4>)
            auto next_it = it;
            ++next_it;
            if (next_it != rel.end()) {
                std::string game_folder = it->u8string();
                std::string mapped_folder = game_folder;

                auto alias_it = cfg.aliases.find(to_lower_ascii(game_folder));
                if (alias_it != cfg.aliases.end()) {
                    mapped_folder = alias_it->second;
                }

                return fs::u8path(cfg.output_root) / fs::u8path(mapped_folder);
            }
        }
    }

    return default_dir;
}

// ============================================================================
// libmpv Callbacks & Helpers
// ============================================================================
static void* get_proc_address_mpv(void* fn_ctx, const char* name) {
    (void)fn_ctx;
    return SDL_GL_GetProcAddress(name);
}

static void on_mpv_render_update(void* ctx) {
    (void)ctx;
    SDL_Event event{};
    event.type = g_wakeup_on_mpv_render_update;
    SDL_PushEvent(&event);
}

static void on_mpv_wakeup(void* ctx) {
    (void)ctx;
    SDL_Event event{};
    event.type = g_wakeup_on_mpv_events;
    SDL_PushEvent(&event);
}

static void seek_exact(AppState& app, float target_sec) {
    if (!app.file_loaded) return;
    std::string sec_str = format_seconds_Arg(target_sec);
    const char* cmd[]   = {"seek", sec_str.c_str(), "absolute", "exact", nullptr};
    mpv_command_async(app.mpv, 0, cmd);
}

static void seek_relative(AppState& app, float delta_sec) {
    if (!app.file_loaded) return;
    float target = std::clamp(static_cast<float>(app.time_pos) + delta_sec, 0.0f, static_cast<float>(app.duration));
    if (app.loop_clip && app.trim_end > app.trim_start) {
        target = std::clamp(target, app.trim_start, app.trim_end);
    }
    app.time_pos = target;
    seek_exact(app, target);
}

static void frame_step(AppState& app, bool forward) {
    if (!app.file_loaded) return;
    const char* cmd[] = {forward ? "frame-step" : "frame-back-step", nullptr};
    mpv_command_async(app.mpv, 0, cmd);
}

// Synchronizes mpv's native A-B loop points with our trim_start / trim_end when loop_clip is active
static void sync_ab_loop(AppState& app) {
    if (!app.file_loaded) return;

    if (app.loop_clip && app.trim_end > app.trim_start) {
        double a = static_cast<double>(app.trim_start);
        double b = static_cast<double>(app.trim_end);
        mpv_set_property_async(app.mpv, 0, "ab-loop-a", MPV_FORMAT_DOUBLE, &a);
        mpv_set_property_async(app.mpv, 0, "ab-loop-b", MPV_FORMAT_DOUBLE, &b);

        // If playhead is currently outside the trim range, jump inside immediately
        if (app.time_pos < a - 0.05 || app.time_pos > b + 0.05) {
            app.time_pos = a;
            seek_exact(app, app.trim_start);
        }
    } else {
        const char* no_val = "no";
        mpv_set_property_async(app.mpv, 0, "ab-loop-a", MPV_FORMAT_STRING, &no_val);
        mpv_set_property_async(app.mpv, 0, "ab-loop-b", MPV_FORMAT_STRING, &no_val);
    }
}

static void set_trim_start_here(AppState& app) {
    if (!app.file_loaded) return;
    app.trim_start = static_cast<float>(app.time_pos);
    if (app.trim_end < app.trim_start) {
        app.trim_end = app.trim_start;
    }
    sync_ab_loop(app);
}

static void set_trim_end_here(AppState& app) {
    if (!app.file_loaded) return;
    app.trim_end = static_cast<float>(app.time_pos);
    if (app.trim_start > app.trim_end) {
        app.trim_start = app.trim_end;
    }
    sync_ab_loop(app);
}

static void load_video_file(AppState& app, const std::string& filepath) {
    app.current_file = filepath;
    app.file_loaded  = true;
    app.time_pos     = 0.0;
    app.duration     = 0.0;
    app.trim_start   = 0.0f;
    app.trim_end     = 0.0f;
    app.loop_clip    = false;
    app.export_status_msg.clear();

    fs::path out_dir = resolve_output_directory(app.config, fs::u8path(filepath));
    app.target_output_dir_display = out_dir.u8string();

    const char* no_val = "no";
    mpv_set_property_async(app.mpv, 0, "ab-loop-a", MPV_FORMAT_STRING, &no_val);
    mpv_set_property_async(app.mpv, 0, "ab-loop-b", MPV_FORMAT_STRING, &no_val);

    const char* cmd[] = {"loadfile", filepath.c_str(), nullptr};
    mpv_command_async(app.mpv, 0, cmd);

    int flag = 0;
    mpv_set_property_async(app.mpv, 0, "pause", MPV_FORMAT_FLAG, &flag);
}

static void toggle_pause(AppState& app) {
    if (!app.file_loaded) return;
    int new_pause = app.paused ? 0 : 1;
    mpv_set_property_async(app.mpv, 0, "pause", MPV_FORMAT_FLAG, &new_pause);
}

// ============================================================================
// Asynchronous FFmpeg Export
// ============================================================================
static void start_export_async(AppState& app, ExportPreset preset) {
    if (app.is_exporting.load() || !app.file_loaded || app.current_file.empty()) {
        return;
    }

    std::string suffix;
    std::string args;
    std::string label;

    switch (preset) {
        case ExportPreset::Discord1080p:
            suffix = "_compressed_1080.mp4";
            args   = FFMPEG_ARGS_DISCORD_1080P;
            label  = "Exporting Discord 1080p (x264 slower)...";
            break;
        case ExportPreset::Discord720p:
            suffix = "_compressed_720.mp4";
            args   = FFMPEG_ARGS_DISCORD_720P;
            label  = "Exporting Discord 720p (x264 slower)...";
            break;
        case ExportPreset::StreamCopy:
            suffix = "_trimmed.mp4";
            args   = FFMPEG_ARGS_LOSSLESS_COPY;
            label  = "Exporting Fast Stream Copy...";
            break;
    }

    fs::path in_path = fs::u8path(app.current_file);
    fs::path out_dir = resolve_output_directory(app.config, in_path);

    // Ensure target output directory exists (e.g., auto-creates D:\Clips\<New Game>\)
    std::error_code ec;
    fs::create_directories(out_dir, ec);

    fs::path out_path = out_dir / (in_path.stem().u8string() + suffix);

    replace_all(args, "{start}", format_seconds_Arg(app.trim_start));
    replace_all(args, "{end}", format_seconds_Arg(app.trim_end));
    replace_all(args, "{input}", in_path.u8string());
    replace_all(args, "{output}", out_path.u8string());

    std::string full_cmd    = "ffmpeg.exe " + args;
    std::string out_display = out_path.u8string();

    app.is_exporting.store(true);
    app.export_Result.store(0);
    app.export_status_msg = label;

    std::thread([&app, full_cmd, out_display]() {
        std::wstring wcmd = utf8_to_wide(full_cmd);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        BOOL launched = CreateProcessW(
            nullptr,
            wcmd.data(), // Command line (modifiable buffer required by CreateProcessW)
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi
        );

        int result = -1;
        if (launched) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD exit_code = 1;
            if (GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code == 0) {
                result = 1;
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }

        app.export_status_msg = (result == 1)
            ? ("Saved -> " + out_display)
            : "Export Failed (check ffmpeg in PATH)";
        app.export_Result.store(result);
        app.is_exporting.store(false);

        SDL_Event ev{};
        ev.type = g_wakeup_on_export_done;
        SDL_PushEvent(&ev);
    }).detach();
}

// ============================================================================
// Main Entry Point (Pure Windows GUI Subsystem - No black console popup)
// ============================================================================
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        return 1;
    }

    // OpenGL 3.2 Core Profile
    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow(
        "Clipski - Video Trimmer",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1280,
        720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!window) {
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    // Enable drag-and-drop file loading
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    // Register SDL custom wakeup events
    g_wakeup_on_mpv_render_update = SDL_RegisterEvents(1);
    g_wakeup_on_mpv_events        = SDL_RegisterEvents(1);
    g_wakeup_on_export_done       = SDL_RegisterEvents(1);

    // Initialize Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // Disable imgui.ini creation
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Initialize AppState & load clipski.ini routing rules
    AppState app{};
    load_routing_config(app.config);

    // Initialize libmpv
    app.mpv = mpv_create();
    if (!app.mpv) {
        return 1;
    }

    // Core requirement: hwdec=auto and loop=inf
    mpv_set_option_string(app.mpv, "vo", "libmpv");
    mpv_set_option_string(app.mpv, "hwdec", "auto");
    mpv_set_option_string(app.mpv, "loop-file", "inf");
    mpv_set_option_string(app.mpv, "keep-open", "yes");

    if (mpv_initialize(app.mpv) < 0) {
        return 1;
    }

    // Observe properties asynchronously so UI state stays synchronized
    mpv_observe_property(app.mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(app.mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(app.mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_set_wakeup_callback(app.mpv, on_mpv_wakeup, nullptr);

    // Initialize mpv OpenGL render context
    mpv_opengl_init_params gl_init_params{get_proc_address_mpv, nullptr};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    if (mpv_render_context_create(&app.mpv_gl, app.mpv, params) < 0) {
        return 1;
    }

    mpv_render_context_set_update_callback(app.mpv_gl, on_mpv_render_update, nullptr);

    // Parse Windows command-line argument (UTF-16 -> UTF-8 safe for any file path)
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        if (wargc > 1 && wargv[1] && wargv[1][0] != L'\0') {
            load_video_file(app, wide_to_utf8(wargv[1]));
        }
        LocalFree(wargv);
    }

    bool running = true;
    bool redraw  = true;

    while (running) {
        SDL_Event event;
        // Event-driven sleep: wait indefinitely when idle; wake only on input, window events, or mpv callbacks
        int wait_status = redraw ? SDL_PollEvent(&event) : SDL_WaitEvent(&event);

        if (wait_status) {
            do {
                ImGui_ImplSDL2_ProcessEvent(&event);

                if (event.type == SDL_QUIT) {
                    running = false;
                } else if (event.type == SDL_WINDOWEVENT) {
                    if (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                        event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        event.window.event == SDL_WINDOWEVENT_RESIZED) {
                        redraw = true;
                    }
                } else if (event.type == SDL_DROPFILE) {
                    char* dropped_file = event.drop.file;
                    if (dropped_file) {
                        load_video_file(app, dropped_file);
                        SDL_free(dropped_file);
                        redraw = true;
                    }
                } else if (event.type == SDL_KEYDOWN && !io.WantCaptureKeyboard) {
                    SDL_Keycode key = event.key.keysym.sym;
                    bool shift = (event.key.keysym.mod & KMOD_SHIFT) != 0;

                    if (!event.key.repeat) {
                        if (key == SDLK_SPACE) {
                            toggle_pause(app);
                            redraw = true;
                        } else if (key == SDLK_s || key == SDLK_i || key == SDLK_LEFTBRACKET) {
                            set_trim_start_here(app);
                            redraw = true;
                        } else if (key == SDLK_e || key == SDLK_o || key == SDLK_RIGHTBRACKET) {
                            set_trim_end_here(app);
                            redraw = true;
                        } else if (key == SDLK_l || key == SDLK_c) {
                            app.loop_clip = !app.loop_clip;
                            sync_ab_loop(app);
                            redraw = true;
                        }
                    }

                    // Repeatable navigation hotkeys (Arrow keys & , / . frame step)
                    if (key == SDLK_LEFT) {
                        seek_relative(app, shift ? -5.0f : -1.0f);
                        redraw = true;
                    } else if (key == SDLK_RIGHT) {
                        seek_relative(app, shift ? 5.0f : 1.0f);
                        redraw = true;
                    } else if (key == SDLK_COMMA) {
                        frame_step(app, false);
                        redraw = true;
                    } else if (key == SDLK_PERIOD) {
                        frame_step(app, true);
                        redraw = true;
                    }
                } else if (event.type == g_wakeup_on_mpv_render_update) {
                    uint64_t flags = mpv_render_context_update(app.mpv_gl);
                    if (flags & MPV_RENDER_UPDATE_FRAME) {
                        redraw = true;
                    }
                } else if (event.type == g_wakeup_on_mpv_events) {
                    while (true) {
                        mpv_event* mp_event = mpv_wait_event(app.mpv, 0);
                        if (mp_event->event_id == MPV_EVENT_NONE) break;

                        if (mp_event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
                            auto* prop = static_cast<mpv_event_property*>(mp_event->data);
                            if (std::strcmp(prop->name, "time-pos") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                                app.time_pos = *static_cast<double*>(prop->data);
                                // Safety clamp if loop_clip is enabled and playhead drifted past trim_end
                                if (app.loop_clip && app.trim_end > app.trim_start) {
                                    if (app.time_pos > static_cast<double>(app.trim_end) + 0.08) {
                                        seek_exact(app, app.trim_start);
                                    }
                                }
                                redraw = true;
                            } else if (std::strcmp(prop->name, "duration") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                                double new_dur = *static_cast<double*>(prop->data);
                                app.duration = new_dur;
                                if (app.trim_end <= 0.0f || app.trim_end > static_cast<float>(new_dur)) {
                                    app.trim_end = static_cast<float>(new_dur);
                                }
                                redraw = true;
                            } else if (std::strcmp(prop->name, "pause") == 0 && prop->format == MPV_FORMAT_FLAG) {
                                app.paused = (*static_cast<int*>(prop->data)) != 0;
                                redraw = true;
                            }
                        }
                    }
                } else if (event.type == g_wakeup_on_export_done) {
                    redraw = true;
                } else if (event.type == SDL_MOUSEMOTION ||
                           event.type == SDL_MOUSEBUTTONDOWN ||
                           event.type == SDL_MOUSEBUTTONUP ||
                           event.type == SDL_MOUSEWHEEL) {
                    redraw = true;
                }
            } while (SDL_PollEvent(&event));
        }

        if (!redraw) {
            continue;
        }
        redraw = false;

        // Query drawable size for high-DPI accuracy
        int draw_w = 0, draw_h = 0;
        SDL_GL_GetDrawableSize(window, &draw_w, &draw_h);

        // Render video frame via libmpv directly to default OpenGL framebuffer
        mpv_opengl_fbo mpfbo{0, draw_w, draw_h, 0};
        int flip_y = 1;
        mpv_render_param render_params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };
        mpv_render_context_render(app.mpv_gl, render_params);

        // Start Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Bottom-docked overlay
        const float overlay_height = 114.0f;
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - overlay_height));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, overlay_height));
        ImGui::SetNextWindowBgAlpha(0.84f);

        ImGuiWindowFlags win_flags = ImGuiWindowFlags_NoTitleBar |
                                     ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoCollapse |
                                     ImGuiWindowFlags_NoSavedSettings;

        if (ImGui::Begin("##BottomOverlay", nullptr, win_flags)) {
            float max_dur = static_cast<float>(app.duration > 0.0 ? app.duration : 1.0);

            // Row 1: Play/Pause button + Loop Clip Toggle + Playback Scrubber
            if (ImGui::Button(app.paused ? " Play (Space) " : " Pause (Space) ", ImVec2(120.0f, 0.0f))) {
                toggle_pause(app);
            }
            ImGui::SameLine();

            if (ImGui::Checkbox("Loop Clip (L)", &app.loop_clip)) {
                sync_ab_loop(app);
            }
            ImGui::SameLine();

            // When "Loop Clip" is checked, bound the scrubber to the clip range
            float current_sec = static_cast<float>(app.time_pos);
            float scrub_min   = (app.loop_clip && app.trim_end > app.trim_start) ? app.trim_start : 0.0f;
            float scrub_max   = (app.loop_clip && app.trim_end > app.trim_start) ? app.trim_end   : max_dur;

            std::string time_label = format_time(app.time_pos) + " / " + format_time(app.duration) +
                                     (app.loop_clip ? "  [CLIP PREVIEW MODE]" : "");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##Scrubber", &current_sec, scrub_min, scrub_max, time_label.c_str())) {
                app.time_pos = current_sec;
                seek_exact(app, current_sec);
            }

            // Draw visual trim markers on top of the scrubber bar when viewing the full timeline
            if (!app.loop_clip && app.duration > 0.0) {
                ImVec2 item_min = ImGui::GetItemRectMin();
                ImVec2 item_max = ImGui::GetItemRectMax();
                float  width    = item_max.x - item_min.x;
                float  x_start  = item_min.x + (app.trim_start / max_dur) * width;
                float  x_end    = item_min.x + (app.trim_end / max_dur) * width;

                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                // Highlight selected trim region in subtle green tint + solid start/end bracket lines
                draw_list->AddRectFilled(
                    ImVec2(x_start, item_min.y),
                    ImVec2(x_end, item_max.y),
                    IM_COL32(80, 220, 100, 45)
                );
                draw_list->AddLine(ImVec2(x_start, item_min.y), ImVec2(x_start, item_max.y), IM_COL32(80, 255, 100, 220), 2.0f);
                draw_list->AddLine(ImVec2(x_end, item_min.y), ImVec2(x_end, item_max.y), IM_COL32(255, 100, 80, 220), 2.0f);
            }

            // Row 2: Trim Range Control + Quick-Set Buttons (S and E)
            if (ImGui::Button("Set Start (S)", ImVec2(105.0f, 0.0f))) {
                set_trim_start_here(app);
            }
            ImGui::SameLine();
            if (ImGui::Button("Set End (E)", ImVec2(105.0f, 0.0f))) {
                set_trim_end_here(app);
            }
            ImGui::SameLine();

            ImGui::SetNextItemWidth(-1.0f);
            float speed = std::max(0.01f, max_dur / 1000.0f);
            if (ImGui::DragFloatRange2(
                    "##TrimRange",
                    &app.trim_start,
                    &app.trim_end,
                    speed,
                    0.0f,
                    max_dur,
                    "Start: %.2fs",
                    "End: %.2fs",
                    ImGuiSliderFlags_AlwaysClamp)) {
                if (app.trim_start > app.trim_end) {
                    std::swap(app.trim_start, app.trim_end);
                }
                sync_ab_loop(app);
            }

            // Row 3: Export Buttons (Discord 1080p, Discord 720p, Fast Lossless Copy) + Status
            bool can_export = app.file_loaded && !app.is_exporting.load() && (app.trim_end > app.trim_start);
            if (!can_export) ImGui::BeginDisabled();
            if (ImGui::Button("Save Discord 1080p", ImVec2(145.0f, 0.0f))) {
                start_export_async(app, ExportPreset::Discord1080p);
            }
            ImGui::SameLine();
            if (ImGui::Button("Save Discord 720p", ImVec2(140.0f, 0.0f))) {
                start_export_async(app, ExportPreset::Discord720p);
            }
            ImGui::SameLine();
            if (ImGui::Button("Save Raw Copy", ImVec2(120.0f, 0.0f))) {
                start_export_async(app, ExportPreset::StreamCopy);
            }
            if (!can_export) ImGui::EndDisabled();

            ImGui::SameLine();
            if (app.is_exporting.load()) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s", app.export_status_msg.c_str());
            } else if (!app.export_status_msg.empty()) {
                ImVec4 color = (app.export_Result.load() == 1)
                                   ? ImVec4(0.3f, 1.0f, 0.4f, 1.0f)
                                   : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
                ImGui::TextColored(color, "%s", app.export_status_msg.c_str());
            } else if (!app.file_loaded) {
                ImGui::TextDisabled("Right-click a video -> Trim with Clipski (or drag & drop here)");
            } else {
                float clip_len = std::max(0.0f, app.trim_end - app.trim_start);
                ImGui::TextDisabled("Clip: %.2fs  |  Out -> %s\\", clip_len, app.target_output_dir_display.c_str());
            }
        }
        ImGui::End();

        // Render ImGui overlay on top of the video
        ImGui::Render();
        glViewport(0, 0, draw_w, draw_h);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);

        // Inform libmpv that swap buffers completed (needed for smooth video timing)
        mpv_render_context_report_swap(app.mpv_gl);
    }

    // Cleanup resources
    mpv_render_context_free(app.mpv_gl);
    mpv_terminate_destroy(app.mpv);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
