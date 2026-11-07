// The settings screen is the phone's own, not the game's: DefJamActivity draws it in Java (SettingsDialog.java), so a
// value is read once when the panel opens and written one at a time as it is changed, across JNI rather than every
// frame. The gear on the on-screen controls (TouchControls.java) opens it. The controls' own JNI is here too.
#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "host.h"

namespace {

// the order the dialog's values are in, the same numbers SettingsDialog.java has
enum Key : int {
    kResolution, kAntiAlias, kAniso, kVsync, kFrameSkip, kAspect, kFpsCounter, kHdr, kHdrAvailable, kControlsOpacity,
    kOutWidth, kOutHeight, kCameraSensitivity, kCameraMode, kFrameRate, kDisableVenueFilter,
    kScreenFilter, kBrightness, kKeyCount
};

// the one index the panel shows for the two settings anti-aliasing is made of: 0 off, 1 SMAA, 2 MSAA 2X, 3 MSAA 4X
int AntiAliasIndex(const game::Options& o) {
    if (o.msaa >= 4) return 3;
    if (o.msaa >= 2) return 2;
    return o.smaa ? 1 : 0;
}

// the names for the settings log, in Key order
const char* const kKeyNames[] = {
    "resolution", "anti_alias", "aniso", "vsync", "frame_skip", "aspect", "fps_counter", "hdr", "hdr_available",
    "controls_opacity", "out_width", "out_height", "camera_sensitivity", "camera_mode", "frame_rate",
    "disable_venue_filter", "screen_filter", "brightness",
};

}  // namespace

void host::ControlsSync(bool pad) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!env) return;
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!activity) return;
    jclass cls = env->GetObjectClass(activity);
    if (!cls) return;
    jmethodID mid = env->GetMethodID(cls, "controlsSync", "(Z)V");
    if (mid) env->CallVoidMethod(activity, mid, jboolean(pad));
    env->DeleteLocalRef(cls);
}

// the panel's values as one array, read when it opens
extern "C" JNIEXPORT jintArray JNICALL Java_com_datfooldive_defjam_DefJamActivity_nativeSettings(JNIEnv* env, jclass) {
    const app::Settings& s = app::Get();
    const game::Options& o = game::Opt();
    const app::Mode out = app::Output();
    jint v[kKeyCount];
    v[kResolution] = jint(s.render_percent);
    v[kAntiAlias] = AntiAliasIndex(o);
    v[kAniso] = jint(s.aniso);
    v[kVsync] = s.vsync ? 1 : 0;
    v[kFrameSkip] = o.frame_skip ? 1 : 0;
    v[kAspect] = o.aspect ? 1 : 0;
    v[kFpsCounter] = o.fps_counter ? 1 : 0;
    v[kHdr] = o.hdr_output ? 1 : 0;
    v[kHdrAvailable] = game::g_hdr_display.load() ? 1 : 0;
    v[kControlsOpacity] = jint(o.controls_opacity);
    v[kOutWidth] = jint(out.width);
    v[kOutHeight] = jint(out.height);
    v[kCameraSensitivity] = jint(o.camera_sensitivity);
    v[kCameraMode] = jint(o.camera_mode);
    v[kFrameRate] = jint(o.frame_rate);
    v[kDisableVenueFilter] = o.disable_venue_filter ? 1 : 0;
    v[kScreenFilter] = jint(o.screen_filter);
    v[kBrightness] = jint(o.brightness);
    jintArray arr = env->NewIntArray(kKeyCount);
    if (arr) env->SetIntArrayRegion(arr, 0, kKeyCount, v);
    return arr;
}

// one value as the panel moves it; commit writes the file, which a slider only does when it is let go
extern "C" JNIEXPORT void JNICALL Java_com_datfooldive_defjam_DefJamActivity_nativeSettingsSet(JNIEnv*, jclass, jint key,
                                                                                       jint value, jboolean commit) {
    app::Settings& s = app::Get();
    game::Options& o = game::Opt();
    switch (key) {
        case kResolution: s.render_percent = uint32_t(std::clamp<jint>(value, 20, 200)); break;
        case kAntiAlias: {
            const int i = std::clamp<jint>(value, 0, 3);
            o.smaa = i == 1;
            o.msaa = i == 2 ? 2 : i == 3 ? 4 : 0;
            break;
        }
        case kAniso: s.aniso = uint32_t(std::clamp<jint>(value, 0, 16)); break;
        case kVsync: s.vsync = value != 0; break;
        case kFrameSkip: o.frame_skip = value != 0; break;
        case kAspect: o.aspect = value ? 1 : 0; break;
        case kFpsCounter: o.fps_counter = value != 0; break;
        case kHdr: o.hdr_output = value != 0 && game::g_hdr_display.load(); break;
        case kControlsOpacity: o.controls_opacity = uint32_t(std::clamp<jint>(value, 20, 100)); break;
        case kCameraSensitivity: o.camera_sensitivity = uint32_t(std::clamp<jint>(value, 50, 300)); break;
        case kCameraMode: o.camera_mode = uint32_t(std::clamp<jint>(value, 0, 2)); break;
        case kFrameRate: o.frame_rate = uint32_t(std::clamp<jint>(value, 0, 1)); break;
        case kDisableVenueFilter: o.disable_venue_filter = value != 0; break;
        case kScreenFilter: o.screen_filter = uint32_t(std::clamp<jint>(value, 0, 5)); break;
        case kBrightness: o.brightness = uint32_t(std::clamp<jint>(value, 50, 200)); break;
        default: return;
    }
    if (commit) {
        LOGR("Setting: %s = %d", kKeyNames[key], int(value));
        app::Save();
    }
}

// the settings back to a fresh install's, the controls' layout with them
extern "C" JNIEXPORT void JNICALL Java_com_datfooldive_defjam_DefJamActivity_nativeSettingsDefaults(JNIEnv*, jclass) {
    app::Settings& s = app::Get();
    game::Options& o = game::Opt();
    s.render_percent = 67;
    s.aniso = 0;
    s.vsync = true;
    o.smaa = false;
    o.msaa = 0;
    o.frame_skip = false;
    o.disable_venue_filter = true;
    o.screen_filter = 0;
    o.brightness = 120;
    o.aspect = 1;
    o.fps_counter = true;
    o.hdr_output = false;
    o.controls_opacity = 100;
    o.camera_sensitivity = game::Options{}.camera_sensitivity;
    o.camera_mode = game::Options{}.camera_mode;
    o.frame_rate = game::Options{}.frame_rate;
    touch::ResetUnits();
}

// True while the game still counts itself in the background. DefJamActivity asks a moment after it regains the window
// focus: a rapid surface loss on resume can leave SDL short of a ready surface, so it never moves the app back to the
// front (handleNativeState needs mIsSurfaceReady) and the game hangs in its background wait. Still background then
// means the Activity is in front but the surface did not come back, and the Java side makes a new one.
extern "C" JNIEXPORT jboolean JNICALL Java_com_datfooldive_defjam_DefJamActivity_nativeStuckInBackground(JNIEnv*,
                                                                                                         jclass) {
    return host::Foreground() ? JNI_FALSE : JNI_TRUE;
}

// the on-screen controls (TouchControls.java): what the fingers hold, as it changes
extern "C" JNIEXPORT void JNICALL Java_com_datfooldive_defjam_TouchControls_nativeTouchPad(JNIEnv*, jclass, jint buttons,
                                                                                    jint analog, jfloat lx, jfloat ly,
                                                                                    jfloat rx, jfloat ry) {
    touch::Set(uint32_t(buttons), uint32_t(analog), lx, ly, rx, ry);
}

extern "C" JNIEXPORT void JNICALL Java_com_datfooldive_defjam_TouchControls_nativeTouchSwipe(JNIEnv*, jclass, jfloat dx,
                                                                                      jfloat dy) {
    touch::Swipe(dx, dy);
}

// their layout: x, y and size for each control in touch.cpp's Unit order
extern "C" JNIEXPORT jfloatArray JNICALL Java_com_datfooldive_defjam_TouchControls_nativeControls(JNIEnv* env, jclass) {
    const int n = touch::Units();
    std::vector<jfloat> v(size_t(n) * 3);
    for (int i = 0; i < n; i++) {
        float x = 0, y = 0, scale = 1;
        touch::GetUnit(i, x, y, scale);
        v[size_t(i) * 3] = x;
        v[size_t(i) * 3 + 1] = y;
        v[size_t(i) * 3 + 2] = scale;
    }
    jfloatArray arr = env->NewFloatArray(jsize(v.size()));
    if (arr) env->SetFloatArrayRegion(arr, 0, jsize(v.size()), v.data());
    return arr;
}

// one control moved or sized in the layout editor; commit writes the file, as a drag or a size bar is let go
extern "C" JNIEXPORT void JNICALL Java_com_datfooldive_defjam_TouchControls_nativeControlSet(JNIEnv*, jclass, jint unit,
                                                                                      jfloat x, jfloat y,
                                                                                      jfloat scale, jboolean commit) {
    touch::SetUnit(unit, x, y, scale, commit);
}

extern "C" JNIEXPORT void JNICALL Java_com_datfooldive_defjam_TouchControls_nativeControlsReset(JNIEnv*, jclass) {
    touch::ResetUnits();
}
