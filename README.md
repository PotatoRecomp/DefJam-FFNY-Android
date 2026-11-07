# Def Jam: Fight for NY for Android

An unofficial Android (arm64) port of the original Xbox version of *Def Jam: Fight for NY*, made with the
[XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) SDK. The game's x86 code in `default.xbe` is statically
recompiled to C++ on a PC, then built for Android together with the SDK's portable runtime and this repo's host code.
Drawing goes through Vulkan, and windowing, input and audio through SDL3. No emulator is involved.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the
> game to install or build.

## Status

Work in progress.

## Installation

Android 10 (API 29) or newer, arm64, with a **Vulkan 1.1** driver or newer.

1. Install the APK.
2. Open **Def Jam Android** and allow **all files access** when it asks (on Android 10, the **storage** permission).
3. Copy the game into `/storage/emulated/0/DefJam`, or into a `DefJam` folder at the top of your SD card: the USA
   release (title id `0x45410049`), either as a disc image (`.iso`) or as `default.xbe` with its data folders beside it.
   The app creates the folder once access is allowed, and starts the game as soon as it finds the files. A copy in the
   app's own `Android/data/com.datfooldive.defjam/files/assets` works too.

The saves (`hdd`) live in the game's folder, beside its files, so uninstalling the app does not take the progress with
it. `settings.toml` stays in the app's own files folder.

Errors and crashes go to `defjam.log` in the game's `DefJam` folder, made at a run's first error (a run without any
leaves the last one's file as it was), with the phone's model and Android version at the top: that is the file to send
with a bug report. The release also writes its Report and Error lines to logcat (`adb logcat -s defjam`).

## Vulkan

The renderer (`native/vulkan.cpp`) needs Vulkan 1.1 and takes what the device offers above it, chosen at run time and
reported in the log:

| | Vulkan 1.1 | above it |
| --- | --- | --- |
| drawing | render passes | dynamic rendering (1.3, or `VK_KHR_dynamic_rendering`) |
| CPU and GPU in step | fences | timeline semaphores (1.2) |
| topology and strides | in the pipelines | extended dynamic state, where the driver gets it right |

PowerVR (vendor `0x1010`: Rogue GE8xxx, BXM, DXT) takes what its driver offers as the others do, with topology and
strides in the pipelines, depth in D32S8 where it has no D24S8, and the point size written by the vertex shaders.

Entry points past 1.1 are looked up on the device, since the NDK's `libvulkan.so` for API 29 exports 1.1 only, and
VMA is held to 1.1.

## Controls

A controller works as on the Xbox, with its rumble; while one is plugged in the on-screen controls are hidden, the gear
staying for the panel. A controller some phones' game services hand over as a virtual device is read by the activity
and plays port 1 all the same (`native/loose.cpp`). A keyboard works as port 1's pad (keys in
`src/common/keyboard.cpp`).

The on-screen controls are the Xbox pad's, where the pad has them: the left stick, the **A B X Y** diamond, White and
Black above it, the triggers, the stick clicks, the D-pad, START and BACK, and the right stick under a finger on the
right. They are thin light outlines with next to no backing, so the fight shows through. The face buttons carry the
pad's letters in its colours and the triggers their names: a fighter's moves depend on the stance and what is held with
them, so no single icon says what a button does. They are an Android view over the game (`TouchControls.java`) that
draws each control from an image, normal and held (`res/drawable-nodpi/ctl_*.png`, made by `tools/controls_art.py`),
and sends what the fingers hold to the game over JNI (`native/touch.cpp`): the game's thread draws nothing for them.
The **gear**, top middle, opens the settings panel; **Move controls** in it places each control, and a size bar there
sizes the one last taken, 50 to 150%.

## Threads

The two locks the runtime holds around the game's code (the resource library's and the dispatch level's) are let go
while a thread blocks in the kernel and give up after a few milliseconds, so neither can freeze the game. On Android the
game's threads run on any core: held to one, its main thread shared a core with the sound and timer threads and the
menus fell near 15 FPS.

## Settings

The gear opens the settings panel, the activity's own (`SettingsDialog.java`), over the game, which keeps running behind
it, in tabs: Graphics, Performance, Display and Controls. A value is applied as it changes and written to
`settings.toml` (`native/settings_bridge.cpp`): render resolution, anti-aliasing (SMAA, MSAA), anisotropic filtering,
vertical sync, frame skip, a 30 FPS lock, the aspect ratio, the FPS counter, HDR, and the on-screen controls' opacity,
right stick mode and sensitivity.

The 30 FPS lock draws every other frame while the game keeps its 60 Hz logic, so the fights do not slow to half speed.

The aspect ratio is ORIGINAL, the TV's shape with side bars, or WIDESCREEN, the default: the scene's camera is widened
where the game builds its view, as Warriors-Android does (`src/game/hooks.cpp`, `View::SetPerspective`, Hor+, its
culling widening with it), so fights and cutscenes show more at the sides with nothing distorted. The 2D layer (menus,
HUD, loading screens) is left as the game draws it and stretches evenly with the frame: placing its pieces one by one
cut and split Def Jam's layered menus. Choose ORIGINAL for menus exactly as on the console.

## Building

Everything runs on Linux. Requirements: clang, cmake, ninja, git, the Android SDK with NDK `30.0.16138531` and CMake
`3.22.1`, and Java 17 or newer. `local.properties` points at the SDK (`build.sh` writes it from `ANDROID_HOME`,
default `~/Android/Sdk`).

1. Put the game in `assets/`: `default.xbe`, taken from the disc image (the image can stay beside it).
2. Run `./build.sh`. Without `generated/game` it builds the recompiler and recompiles `default.xbe` from
   `config/game.toml` first, then builds `app/build/outputs/apk/release/app-release.apk` with Gradle.
3. Install: `adb install -r app/build/outputs/apk/release/app-release.apk`.

`config/game.toml` is the Linux port's, with its symbols mapped by hand (`BeginPush`, `EndPush`, `SetPalette` and
seven DirectSound buffer functions): keep it, do not regenerate it.

## Layout

| path | |
| --- | --- |
| `native/` | the Android host: `main.cpp` (SDL window, event loop, lifecycle, pacing), `vulkan.cpp` (the Vulkan 1.1+ backend), `vk_shader.cpp` (HLSL to SPIR-V with glslang), `touch.cpp` (what the on-screen controls hold, and their layout in the settings file), `settings_bridge.cpp` (the settings panel's and the controls' JNI), `loose.cpp`, `game_files.cpp`, `system_info.cpp` |
| `src/` | the game's host code, shared with the Linux port, with the Android options, FPS counter and touch input; `src/d3d/render_thread.cpp` is the Android render thread (NEON, frame skip) |
| `sdk/` | XboxXBERecomp with the Android runtime changes, as PES5-Android has it |
| `config/game.toml` | recompiler input, the Linux port's |
| `app/` | the Gradle module: `DefJamActivity`, `SettingsDialog`, `LayoutEditor`, `TouchControls`, `Theme`, `GameImporter`, the storage permission prompts, the icon (the cover art, adaptive: the cover inside the safe zone over a blurred copy of it, WebP) |
| `tools/host/` | the recompiler as a PC tool, built by `build.sh` |

## Credits

- [TGP482](https://github.com/TGP482) for [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp), the recompiler
  and runtime SDK this port is built on.
- The Android host and Vulkan renderer from Black-Android, DW5-Android, Warriors-Android and PES5-Android.
- [SDL](https://github.com/libsdl-org/SDL), [glslang](https://github.com/KhronosGroup/glslang),
  [toml++](https://github.com/marzer/tomlplusplus), [Dear ImGui](https://github.com/ocornut/imgui) and
  [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator).

Def Jam: Fight for NY is © 2004 Electronic Arts. This project is not affiliated with or endorsed by Electronic Arts.
