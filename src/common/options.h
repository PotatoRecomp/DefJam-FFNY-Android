#pragma once
#include <atomic>
#include <cstdint>
#include <string>

// The renderer's own settings. The title has no options screen here, so these are the
// defaults a fresh install would use; they are read every frame by d3d/vulkan_linux.cpp.
namespace game {

struct Options {
    bool bloom = true;        // the glow added over bright parts of the frame
    bool fog = true;
    uint32_t msaa = 0;        // samples a pixel of the scene, 0 off
    bool smaa = false;        // over the finished frame
    bool hdr_output = false;  // HDR10 on a display that offers it
    bool fps_counter = true;     // the frame rate in the top left corner (Android)
    bool frame_skip = false;     // frames left undrawn while the drawing is behind, the game at its speed (Android)
    bool disable_venue_filter = true;  // disables murky dark green venue filter and corner darkening (sub_000D8C40)
    uint32_t aspect = 1;         // the frame's shape on the screen: 0 the TV's, else widescreen: the 3D view as wide as
                                 // the screen and the 2D layer kept 16:9 in the middle (Android); a file from before
                                 // keeps 1 to 3, its other shapes, which all read as widescreen
    uint32_t controls_opacity = 100;  // the on-screen controls' opacity, 20 to 100% (Android)
    uint32_t camera_sensitivity = 300;  // the on-screen right stick, 50 to 300%: at 100 a full push is a tenth of the
                                        // screen's short side from where the finger went down (Android)
    uint32_t camera_mode = 1;  // the on-screen right stick: 0 floating, centred where the finger went down; 1 fixed,
                               // a stick of its own the layout editor places; 2 swipe, a drag turns the view (Android)
    uint32_t frame_rate = 0;   // 0 auto, the settings file's cap; 1 held to 30 a second (Android)
    uint32_t screen_filter = 0;  // 0 off, 1 sharpen, 2 vivid, 3 scanlines, 4 CRT, 5 HDR
    uint32_t brightness = 120;   // 50% to 200%, 120% default
    bool cheats[5] = {};       // the CHEATS tab's switches, in common/cheats.cpp's order (Android)
};

inline Options& Opt() {
    static Options options;
    return options;
}

// the frames a second the game is held to: 30 when the panel locks it, the settings file's cap otherwise (0 none). A
// cap of 1 in the file is app::kFpsCapRefresh, the display's refresh rate, which app::Pace reads: 60 here, the rate
// the frame skip's budget and the frame rate counter go by
inline uint32_t FrameCap(uint32_t file_cap) { return Opt().frame_rate == 1 ? 30 : file_cap == 1 ? 60 : file_cap; }

inline std::atomic<bool> g_hdr_display{false};  // the window's display has HDR on, from the renderer

// widescreen: the screen's width over the picture's in its TV shape at the same height, 1 when off; the renderer sets
// it and the camera hook (game/hooks.cpp) widens the scene's view by it, as Warriors-Android does
inline std::atomic<float> g_wide{1.0f};

std::string ResourceLockHolder();  // game/hooks.cpp: the thread holding the title's resource and stream lock

}  // namespace game
