// The Android host: SDL's window and event loop, the app's lifecycle, the settings and folders; the game is described
// by app::kGame (src/common/game_info.cpp), as on Linux, whose host (sdk/runtime/app/main_linux.cpp) this follows
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#include <dlfcn.h>
#include <jni.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <unwind.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include <imgui.h>

#include "app/app.h"
#include "common/options.h"
#include "core/input.h"
#include "core/log.h"
#include "host.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace app {

std::filesystem::path g_game_dir;  // native/game_files.cpp: where the game's files were found

namespace {

SDL_Window* g_window = nullptr;
std::vector<std::string> g_args;
// SDL_RegisterEvents' range: what other threads ask of the window thread
uint32_t g_event_base = 0;
enum : uint32_t { kApplyDisplay, kOpenWindow, kRun, kEventCount };
std::atomic<bool> g_running{true};
std::atomic<bool> g_foreground{true};
// each return to the front: Android hands a new window, whose surface the renderer makes again (host::SurfaceEpoch)
std::atomic<uint32_t> g_surface_epoch{0};
std::mutex g_foreground_lock;
std::condition_variable g_foreground_cv;
std::atomic<bool> g_focus_paused{false};  // the game's thread waits in WaitWhileUnfocused

std::atomic<uint32_t> g_pixel_w{1280};
std::atomic<uint32_t> g_pixel_h{720};
std::atomic<float> g_density{1};
// the screen the render scale is taken from: the last landscape size the window had. Coming back from the background
// Android may show the window portrait or 1 pixel for a moment, and a render scale from that remade every target
std::atomic<uint32_t> g_out_w{1280};
std::atomic<uint32_t> g_out_h{720};

std::mutex g_open_lock;
std::condition_variable g_open_cv;
uint64_t g_open_asked = 0;
uint64_t g_open_done = 0;

bool HasArg(const char* arg) { return std::find(g_args.begin(), g_args.end(), arg) != g_args.end(); }

void PushEvent(uint32_t kind, int32_t code = 0, void* data = nullptr) {
    SDL_Event e{};
    e.type = g_event_base + kind;
    e.user.code = code;
    e.user.data1 = data;
    SDL_PushEvent(&e);
}

// window thread: what other threads read of the window
void UpdateSizes() {
    if (!g_window) return;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    if (w > 1 && h > 1 && w >= h) {
        g_pixel_w = uint32_t(w);
        g_pixel_h = uint32_t(h);
        g_out_w = uint32_t(w);
        g_out_h = uint32_t(h);
    }
    float density = SDL_GetWindowPixelDensity(g_window);
    if (density > 0) g_density = density;
}

ImGuiKey KeyOf(SDL_Keycode k) {
    if (k >= SDLK_A && k <= SDLK_Z) return ImGuiKey(ImGuiKey_A + (k - SDLK_A));
    if (k >= SDLK_0 && k <= SDLK_9) return ImGuiKey(ImGuiKey_0 + (k - SDLK_0));
    switch (k) {
        case SDLK_TAB: return ImGuiKey_Tab;
        case SDLK_LEFT: return ImGuiKey_LeftArrow;
        case SDLK_RIGHT: return ImGuiKey_RightArrow;
        case SDLK_UP: return ImGuiKey_UpArrow;
        case SDLK_DOWN: return ImGuiKey_DownArrow;
        case SDLK_BACKSPACE: return ImGuiKey_Backspace;
        case SDLK_SPACE: return ImGuiKey_Space;
        case SDLK_RETURN: return ImGuiKey_Enter;
        case SDLK_ESCAPE: return ImGuiKey_Escape;
        default: return ImGuiKey_None;
    }
}

int MouseButtonOf(uint8_t b) {
    switch (b) {
        case SDL_BUTTON_LEFT: return 0;
        case SDL_BUTTON_RIGHT: return 1;
        case SDL_BUTTON_MIDDLE: return 2;
        default: return -1;
    }
}

// the event as ImGui's input, in window pixels as the renderer draws it; UiLock held. SDL turns touches into mouse
// events on Android, which is what the panel takes
void FeedImGui(const SDL_Event& e) {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();
    float density = g_density;
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION:
            io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
            io.AddMousePosEvent(e.motion.x * density, e.motion.y * density);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (int b = MouseButtonOf(e.button.button); b >= 0) {
                io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
                io.AddMouseButtonEvent(b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            }
            break;
        case SDL_EVENT_TEXT_INPUT:
            io.AddInputCharactersUTF8(e.text.text);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (ImGuiKey key = KeyOf(e.key.key); key != ImGuiKey_None) io.AddKeyEvent(key, e.type == SDL_EVENT_KEY_DOWN);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            io.AddFocusEvent(e.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
            break;
        default:
            break;
    }
}

// Android's pause and resume, seen as SDL queues them: SDL blocks this thread inside the pump right after queueing
// the pause, so the queue hands it over only after the resume, and the game thread went on drawing into a window
// Android had taken away. A watch runs at the queueing, before the block.
void SetForeground(bool on) {
    {
        std::lock_guard lock(g_foreground_lock);
        if (g_foreground == on) return;
        if (on) g_surface_epoch++;
        g_foreground = on;
    }
    g_foreground_cv.notify_all();
    LOGR("app: %s", on ? "in front" : "in the background");
}

bool Lifecycle(void*, SDL_Event* e) {
    switch (e->type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            SetForeground(false);
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            UpdateSizes();
            SetForeground(true);
            break;
        default:
            break;
    }
    return true;
}

void Handle(const SDL_Event& e) {
    {
        std::lock_guard lock(UiLock());
        FeedImGui(e);
    }
    if (e.type >= g_event_base && e.type < g_event_base + kEventCount) {
        switch (e.type - g_event_base) {
            case kApplyDisplay:
                break;  // Android shows the window full screen and does not resize it
            case kOpenWindow: {
                SDL_ShowWindow(g_window);
                SDL_RaiseWindow(g_window);
                std::lock_guard lock(g_open_lock);
                g_open_done++;
                g_open_cv.notify_all();
                break;
            }
            case kRun: {
                std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(e.user.data1));
                (*fn)();
                break;
            }
        }
        return;
    }
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            LOGR("window: quit event 0x%x", unsigned(e.type));
            g_running = false;
            break;
        case SDL_EVENT_TERMINATING:
            g_running = false;
            // the game's thread may be waiting for the front: it goes on to the exit
            SetForeground(true);
            break;
        case SDL_EVENT_LOW_MEMORY:
            LOGW("app: low memory");
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            input::OnDevice(e.type == SDL_EVENT_GAMEPAD_ADDED, e.gdevice.which);
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            UpdateSizes();
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            // the window has the focus only in front: a return SDL's lifecycle did not tell is taken here
            if (!g_foreground) {
                LOGE("app: the window has the focus while counted in the background: resumed");
                UpdateSizes();
                SetForeground(true);
            }
            break;
        default:
            break;
    }
}

// UTF-8 from a wide string: wchar_t is UTF-32 here
std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) {
        auto u = uint32_t(c);
        if (u < 0x80) {
            s += char(u);
        } else if (u < 0x800) {
            s += char(0xC0 | (u >> 6));
            s += char(0x80 | (u & 0x3F));
        } else if (u < 0x10000) {
            s += char(0xE0 | (u >> 12));
            s += char(0x80 | ((u >> 6) & 0x3F));
            s += char(0x80 | (u & 0x3F));
        } else {
            s += char(0xF0 | (u >> 18));
            s += char(0x80 | ((u >> 12) & 0x3F));
            s += char(0x80 | ((u >> 6) & 0x3F));
            s += char(0x80 | (u & 0x3F));
        }
    }
    return s;
}

// the settings file the port starts with: the screen's own size, two thirds of its height rendered, 60 a second as
// the title's timing counts on (at 30 it runs at half speed)
void WriteDefaultSettings(const std::filesystem::path& path) {
    std::ofstream f(path);
    if (!f) return;
    f << "display_mode = \"borderless\"\n"
         "width = 0\n"
         "height = 0\n"
         "render_percent = 67\n"
         "fps_cap = 60\n"
         "vsync = true\n"
         "anisotropic_filtering = 0\n";
}

}  // namespace

std::string Utf8(const std::wstring& text) { return Narrow(text); }

// the saves were kept in the app's own files folder, which an uninstall wipes: the first run that finds the game
// takes them into the game's folder, so the progress already made is not left behind. True when the game's folder is
// the one to use, false when the old saves could not be moved and stay where they are
bool TakeSaves(const std::filesystem::path& to) {
    std::error_code ec;
    const std::filesystem::path from = ExeDir() / "hdd";
    if (std::filesystem::exists(to, ec)) return true;           // already there, the app's own left alone
    if (!std::filesystem::is_directory(from, ec)) return true;  // a fresh install: made there
    std::filesystem::rename(from, to, ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy(from, to, std::filesystem::copy_options::recursive, ec);
        if (ec) {
            LOGW("saves: %s did not move to %s: %s", from.c_str(), to.c_str(), ec.message().c_str());
            std::error_code ignored;
            std::filesystem::remove_all(to, ignored);  // what was half copied
            return false;
        }
        std::filesystem::remove_all(from, ec);
    }
    LOGI("saves: moved from %s to %s", from.c_str(), to.c_str());
    return true;
}

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = [] {
        const char* internal = SDL_GetAndroidInternalStoragePath();
        if (internal && *internal) return std::filesystem::path(internal);
        return std::filesystem::path(SDL_GetBasePath() ? SDL_GetBasePath() : ".");
    }();
    return dir;
}

int Instance() { return 0; }

void* Window() { return g_window; }

void Quit() {
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
}

void Post(std::function<void()> fn) { PushEvent(kRun, 0, new std::function<void()>(std::move(fn))); }

void ShowError(const std::wstring& text) {
    std::string utf8 = Narrow(text);
    LOGE("error: %s", utf8.c_str());
    host::Toast(utf8.c_str());
    // the toast outlives the process only while it is up: give it a moment, then let the activity close
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

bool AskYesNo(const std::wstring& text) {
    ShowError(text);
    return false;
}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() {}

void OpenGameWindow(bool) {
    std::unique_lock lock(g_open_lock);
    uint64_t ticket = ++g_open_asked;
    PushEvent(kOpenWindow);
    g_open_cv.wait(lock, [&] { return g_open_done >= ticket || !g_running; });
}

Mode Desktop() { return {g_out_w.load(), g_out_h.load()}; }
Mode WindowPixels() { return {g_pixel_w.load(), g_pixel_h.load()}; }

void UiFrame() {
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(g_pixel_w), float(g_pixel_h));
    io.DisplayFramebufferScale = ImVec2(1, 1);
    auto now = clock::now();
    io.DeltaTime = std::max(std::chrono::duration<float>(now - last).count(), 1e-4f);
    last = now;
    // the on-screen D-pad and A move through the panel: port 1's poll takes the touch controls with the keyboard
    input::Pad p;
    for (int port = 0; port < 4 && !p.connected; port++) p = port == 0 ? input::Poll(0) : input::Read(port);
    if (p.connected) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    if (!p.connected) return;
    constexpr uint8_t kPressed = 0x40;
    auto key = [&](ImGuiKey k, bool down) { io.AddKeyEvent(k, down); };
    key(ImGuiKey_GamepadDpadUp, p.buttons & 0x01);
    key(ImGuiKey_GamepadDpadDown, p.buttons & 0x02);
    key(ImGuiKey_GamepadDpadLeft, p.buttons & 0x04);
    key(ImGuiKey_GamepadDpadRight, p.buttons & 0x08);
    key(ImGuiKey_GamepadStart, p.buttons & 0x10);
    key(ImGuiKey_GamepadBack, p.buttons & 0x20);
    key(ImGuiKey_GamepadFaceDown, p.analog[0] > kPressed);
    key(ImGuiKey_GamepadFaceRight, p.analog[1] > kPressed);
    key(ImGuiKey_GamepadFaceLeft, p.analog[2] > kPressed);
    key(ImGuiKey_GamepadFaceUp, p.analog[3] > kPressed);
    key(ImGuiKey_GamepadR1, p.analog[4] > kPressed);
    key(ImGuiKey_GamepadL1, p.analog[5] > kPressed);
    key(ImGuiKey_GamepadL2, p.analog[6] > kPressed);
    key(ImGuiKey_GamepadR2, p.analog[7] > kPressed);
    constexpr float kDead = 8000.0f;
    auto stick = [&](ImGuiKey k, float v) {
        float a = std::clamp((v - kDead) / (32767.0f - kDead), 0.0f, 1.0f);
        io.AddKeyAnalogEvent(k, a > 0.1f, a);
    };
    stick(ImGuiKey_GamepadLStickLeft, -float(p.lx));
    stick(ImGuiKey_GamepadLStickRight, float(p.lx));
    stick(ImGuiKey_GamepadLStickUp, float(p.ly));
    stick(ImGuiKey_GamepadLStickDown, -float(p.ly));
}

namespace {

// the window's display's refresh rate, read again each second (a mode change); 0 when unreadable
uint32_t RefreshRate() {
    using clock = std::chrono::steady_clock;
    static uint32_t rate = 0;
    static clock::time_point next{};
    if (auto now = clock::now(); now >= next) {
        next = now + std::chrono::seconds(1);
        SDL_DisplayID d = g_window ? SDL_GetDisplayForWindow(g_window) : 0;
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(d ? d : SDL_GetPrimaryDisplay());
        rate = mode && mode->refresh_rate > 1 ? uint32_t(mode->refresh_rate + 0.5f) : 0;
    }
    return rate;
}

}  // namespace

namespace {

std::atomic<uint64_t> g_swaps{0};  // the game's Swaps so far, for the freeze watchdog
std::atomic<uint64_t> g_shown{0};  // frames the renderer put on the screen (host::NotePresent)
std::atomic<int> g_acquire_result{0}, g_present_result{0};  // the renderer's last VkResults
// the render thread's current sub-step and when it entered it, for the freeze report (host::NoteRenderPhase)
std::atomic<const char*> g_render_phase{"start"};
std::atomic<int64_t> g_render_phase_ns{0};

// A game that stops swapping in front for kFrozen is written up once in defjam.log: each guest thread's stack, who
// holds the title's resource lock, and each native thread's state (R running, S sleeping) and what it sleeps in.
// That is what tells a loop in the title's code from a wait on a lock or on the renderer
void LogThreads() {
    {
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        const int64_t in_ns = now - g_render_phase_ns.load(std::memory_order_relaxed);
        LOGE("freeze: render thread in '%s' for %lld ms, last acquire %d, present %d, surface epoch %u",
             g_render_phase.load(std::memory_order_relaxed), (long long)(in_ns / 1'000'000),
             g_acquire_result.load(), g_present_result.load(), g_surface_epoch.load());
    }
    for (auto& t : kernel::Threads()) {
        if (!t || !t->ctx) continue;
        LOGE("freeze: guest thread %X", t->id);
        kernel::LogGuestStack(*t->ctx, 48);
    }
    std::error_code ec;
    for (const auto& task : std::filesystem::directory_iterator("/proc/self/task", ec)) {
        std::string name, stat, wchan;
        std::ifstream(task.path() / "comm") >> name;
        std::getline(std::ifstream(task.path() / "stat"), stat);
        std::ifstream(task.path() / "wchan") >> wchan;
        // the state follows the name in parentheses
        size_t close = stat.rfind(')');
        char state = close != std::string::npos && close + 2 < stat.size() ? stat[close + 2] : '?';
        LOGE("freeze: native %s (%s) %c %s", task.path().filename().c_str(), name.c_str(), state, wchan.c_str());
    }
}

void ReportFreeze(uint64_t swaps) {
    LOGE("freeze: no Swap for %d s at swap %llu, in front, resource lock held by %s", 3, (unsigned long long)swaps,
         game::ResourceLockHolder().c_str());
    LogThreads();
    logx::Flush();
}

void StartFreezeWatchdog() {
    std::thread([] {
        using clock = std::chrono::steady_clock;
        constexpr auto kFrozen = std::chrono::seconds(3);
        uint64_t seen = g_swaps, shown = g_shown;
        auto since = clock::now(), shown_since = since, behind_since = since;
        bool reported = false, reported_shown = false;
        int reports = 0;  // a long load without frames looks the same: a few per run at most
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto t = clock::now();
            // Android has the window in front (it has the input focus) while the app still counts itself in the
            // background: a return to the front SDL's lifecycle did not tell, and the game would wait in its Swap for
            // ever with the last picture up
            bool focused = g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS);
            if (g_foreground || !focused) behind_since = t;
            if (t - behind_since >= std::chrono::seconds(2)) {
                LOGE("freeze: in front with the input focus but counted in the background for 2 s: resumed");
                SetForeground(true);
                behind_since = t;
            }
            uint64_t now = g_swaps, now_shown = g_shown;
            // the background, a focus pause and the time before the first frame are no freeze
            bool idle = !g_foreground || g_focus_paused || !now;
            if (now != seen || idle) {
                if (reported && now != seen) {
                    LOGE("freeze: the game swaps again at %llu", (unsigned long long)now);
                    reported = false;
                }
                seen = now;
                since = t;
            } else if (!reported && reports < 3 && t - since >= kFrozen) {
                reported = true;
                reports++;
                ReportFreeze(now);
            }
            // the game swapping and nothing reaching the screen: the picture stands still, the game and its sound go on
            if (now_shown != shown || idle || now == seen) {
                if (reported_shown && now_shown != shown) {
                    LOGE("freeze: frames reach the screen again");
                    reported_shown = false;
                }
                shown = now_shown;
                shown_since = t;
            } else if (!reported_shown && reports < 3 && t - shown_since >= kFrozen) {
                reported_shown = true;
                reports++;
                LOGE("freeze: the game swaps (%llu) and no frame reached the screen for 3 s: acquire %d, present %d, "
                     "foreground %d, surface epoch %u, window %dx%d",
                     (unsigned long long)now, g_acquire_result.load(), g_present_result.load(), int(g_foreground.load()),
                     g_surface_epoch.load(), int(g_pixel_w.load()), int(g_pixel_h.load()));
                logx::Flush();
            }
        }
    }).detach();
}

}  // namespace

void Pace() {
    g_swaps.fetch_add(1, std::memory_order_relaxed);
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    // The game simulates one step per frame and counts on 60 a second, so the simulation is always paced to the file's
    // cap (60, or the refresh rate), never to the 30 FPS lock: at 30 that would run the title at half speed. The 30 FPS
    // lock instead drops every other drawn frame in the render thread, so the picture is 30 while the game stays full
    // speed.
    uint32_t cap = Get().fps_cap;
    // a few under the refresh rate, so a variable refresh display stays in its range and vsync never waits a whole
    // refresh
    constexpr uint32_t kRefreshMargin = 3;
    if (cap == kFpsCapRefresh) cap = RefreshRate() > kRefreshMargin ? RefreshRate() - kRefreshMargin : 0;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(1) < next) std::this_thread::sleep_for(std::chrono::microseconds(500));
    while (clock::now() < next) std::this_thread::yield();
}

// In the background Android takes the window away. The game's thread used to go on into its Swap and wait in the
// renderer for the front, holding the D3D lock and whatever locks of the title it had (the resource library's, the
// streams'): the other threads gave up on those after their patience and ran unlocked, and the game came back frozen
// with the music playing. It waits here instead, at the top of Swap before any lock, as Most Wanted-Android does. The
// sound is left to SDL, which stops Android's audio in the background itself: held here as well, the output came back
// after SDL had started Android's again and filled it in a burst
void WaitWhileUnfocused() {
    if (g_foreground) return;
    LOGR("app: paused in the background");
    g_focus_paused = true;
    kernel::HoldGuestClock(true);
    {
        // a wait as the kernel's: the title's locks the thread holds (src/game/hooks.cpp) and the dispatch level go
        // for it, so the stream threads still running keep them
        kernel::Blocking blocking;
        std::unique_lock lock(g_foreground_lock);
        g_foreground_cv.wait(lock, [] { return g_foreground.load() || !g_running; });
    }
    kernel::HoldGuestClock(false);
    g_focus_paused = false;
    LOGR("app: resumed");
}

bool FocusPaused() { return g_focus_paused; }

// the port has no splash picture and one instance only
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}
bool PrepareGameCode(const GameFiles&) { return true; }

}  // namespace app

namespace host {

SDL_Window* Window() { return app::g_window; }
bool Foreground() { return app::g_foreground; }
uint32_t SurfaceEpoch() { return app::g_surface_epoch; }
std::filesystem::path FilesDir() { return app::ExeDir(); }
std::filesystem::path GameDir() { return app::g_game_dir; }

void NotePresent(int acquire, int present, bool shown) {
    app::g_acquire_result = acquire;
    app::g_present_result = present;
    if (shown) app::g_shown++;
}

void NoteRenderPhase(const char* phase) {
    app::g_render_phase.store(phase ? phase : "idle", std::memory_order_relaxed);
    app::g_render_phase_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count(),
                                 std::memory_order_relaxed);
}

void Toast(const char* text) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!env) return;
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!activity) return;
    jclass cls = env->GetObjectClass(activity);
    if (cls) {
        jmethodID mid = env->GetStaticMethodID(cls, "showToast", "(Ljava/lang/String;)V");
        if (mid) {
            jstring s = env->NewStringUTF(text ? text : "");
            if (s) {
                env->CallStaticVoidMethod(cls, mid, s);
                env->DeleteLocalRef(s);
            }
        }
        env->DeleteLocalRef(cls);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(activity);
}

}  // namespace host


namespace {

// a native frame of the crash backtrace: the library and the offset in it, what ndk-stack and addr2line take
_Unwind_Reason_Code LogFrame(_Unwind_Context* uc, void* arg) {
    int& depth = *static_cast<int*>(arg);
    uintptr_t pc = _Unwind_GetIP(uc);
    if (!pc || depth >= 32) return _URC_END_OF_STACK;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
        const char* lib = std::strrchr(info.dli_fname, '/');
        LOGE("  #%02d %s+0x%zx", depth, lib ? lib + 1 : info.dli_fname,
             size_t(pc - reinterpret_cast<uintptr_t>(info.dli_fbase)));
    } else {
        LOGE("  #%02d %p", depth, reinterpret_cast<void*>(pc));
    }
    depth++;
    return _URC_NO_REASON;
}

// a crash goes to the log file with the guest thread's stack and the native backtrace, then on to Android's own
// handler (the tombstone) as if this one were not there
void OnCrash(int sig, siginfo_t* info, void*) {
    void* at = info ? info->si_addr : nullptr;
    if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
        LOGE("crash: signal %d (%s) at %p on guest thread %X", sig, strsignal(sig), at, t->id);
        if (sig == SIGSEGV || sig == SIGBUS) {
            uintptr_t va = reinterpret_cast<uintptr_t>(at), g = reinterpret_cast<uintptr_t>(kernel::g_base);
            if (va >= g && va - g < (uintptr_t(1) << 32)) LOGE("  guest address %08zX", size_t(va - g));
        }
        kernel::LogGuestStack(*t->ctx, 64);
    } else {
        LOGE("crash: signal %d (%s) at %p", sig, strsignal(sig), at);
    }
    int depth = 0;
    _Unwind_Backtrace(LogFrame, &depth);
    logx::Flush();
    signal(sig, SIG_DFL);
    raise(sig);
}

void InstallCrashHandler() {
    struct sigaction sa{};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
}

// errors and crashes go to defjam.log in the game's folder, beside its files, where a player finds it to send; the
// header says which phone it came from
void LogToGameFolder(const std::filesystem::path& dir) {
    char model[PROP_VALUE_MAX] = "", maker[PROP_VALUE_MAX] = "", release[PROP_VALUE_MAX] = "", sdk[PROP_VALUE_MAX] = "";
    __system_property_get("ro.product.manufacturer", maker);
    __system_property_get("ro.product.model", model);
    __system_property_get("ro.build.version.release", release);
    __system_property_get("ro.build.version.sdk", sdk);
    std::time_t now = std::time(nullptr);
    char date[32];
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::string header = app::Narrow(app::kGame.title) + " (Android), " + date + "\n" + maker + " " + model +
                         ", Android " + release + " (API " + sdk + ")\n";
    logx::ErrorFile((dir / "defjam.log").string(), header);
}

}  // namespace

int main(int argc, char** argv) {
    // SDL sets the activity's orientation from the window it is asked for, over the manifest's sensorLandscape
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    app::g_args.assign(argv + 1, argv + argc);

    std::string title = app::Narrow(app::kGame.title);
    SDL_SetAppMetadata(title.c_str(), nullptr, nullptr);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOGE("SDL video failed: %s", SDL_GetError());
        app::ShowError(L"No display to open the game window on.");
        return 1;
    }
    app::g_event_base = SDL_RegisterEvents(app::kEventCount);
    SDL_AddEventWatch(app::Lifecycle, nullptr);
    app::UpdateSizes();

    std::error_code ec;
    if (!std::filesystem::exists(app::ExeDir() / app::kGame.settings, ec))
        app::WriteDefaultSettings(app::ExeDir() / app::kGame.settings);
    app::Load();
    // 30 a second was the first default, which the title plays in slow motion at; the port has no settings screen,
    // so a 30 can only be that default
    if (app::Get().fps_cap == 30) {
        app::Get().fps_cap = 60;
        app::Save();
    }
    // a full log in the app's internal storage, beside settings.toml: Report and Warn lines (system info, GPU details,
    // game search, settings) go to it as well as logcat, so a problem that is not a hard error still leaves a file.
    // defjam.log in the game's folder stays the error-only file a player finds and sends.
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    LOGR("Started");
    if (app::kGame.init) app::kGame.init();
    app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;
    LogToGameFolder(game.dir);
    InstallCrashHandler();
    if (!app::PrepareGameCode(game)) return 1;
    // the saves go in the game's own folder, beside its files: an uninstall takes the app's folder, not this one.
    // An install that still has them in the app's own files takes them along, and keeps them there if they cannot
    // be moved, so no progress is lost
    std::filesystem::path hdd = game.dir / "hdd";
    if (!app::TakeSaves(hdd)) hdd = app::ExeDir() / "hdd";

    app::g_window = SDL_CreateWindow(title.c_str(), 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!app::g_window) {
        LOGE("SDL window failed: %s", SDL_GetError());
        app::ShowError(L"The game window could not be opened, see defjam.log in the DefJam folder.");
        return 1;
    }
    app::UpdateSizes();
    LOGR("Window: %ux%u", app::g_pixel_w.load(), app::g_pixel_h.load());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendPlatformName = "sdl3 (android)";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

    input::Init();
    touch::Init();
    if (app::kGame.window) app::kGame.window(app::g_window);
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), hdd.string())) {
        app::ShowError(L"The game failed to start, see defjam.log in the DefJam folder.");
        return 1;
    }
    app::StartFreezeWatchdog();

    // Polled, then a short sleep: SDL_WaitEventTimeout on Android pumps its events again each time anything is
    // queued, its own bookkeeping included, and kept this thread at 85% of a big core while the game ran. A touch
    // waits 4 ms at most.
    constexpr auto kIdle = std::chrono::milliseconds(4);
    while (app::g_running) {
        SDL_Event e;
        bool any = false;
        while (app::g_running && SDL_PollEvent(&e)) {
            app::Handle(e);
            any = true;
        }
        if (!any) std::this_thread::sleep_for(kIdle);
    }
    {
        std::lock_guard lock(app::g_open_lock);
        app::g_open_cv.notify_all();
    }
    {
        std::lock_guard lock(app::g_foreground_lock);
        app::g_foreground_cv.notify_all();
    }
    LOGR("Exiting");
    logx::Flush();
    kernel::Shutdown(0);
    return 0;
}
