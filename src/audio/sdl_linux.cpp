// the mixer's stereo output through SDL's audio stream
#include <SDL3/SDL.h>

#include <chrono>
#include <thread>
#include <vector>

#include "audio/audio.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace audio {

namespace {

constexpr uint32_t kOutChannels = 2;
constexpr uint32_t kFrames = kRate / 100;  // the mixer renders 10 ms at a time
constexpr int kChunkBytes = int(kFrames * kOutChannels * sizeof(float));
constexpr uint32_t kMinQueued = 4;   // 40 ms ahead of the device
constexpr uint32_t kMaxQueued = 12;  // 120 ms at most

// the mixer runs on its own thread and keeps the stream a few chunks ahead: rendered inside the device callback it
// waited on the guest's dsound lock and the reverb within AAudio's few-ms deadline, and slow phones crackled
void MixThread(SDL_AudioStream* stream, std::function<void(float*, uint32_t)> render) {
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    std::vector<float> mix(kFrames * kOutChannels);
    uint32_t depth = kMinQueued;
    bool primed = false;
    for (;;) {
        int queued = SDL_GetAudioStreamQueued(stream);
        if (queued < 0) return;
        // a drained stream crackles: keep one chunk more ahead each time, up to kMaxQueued
        if (primed && queued == 0 && depth < kMaxQueued) {
            depth++;
            LOGI("audio: output starved, queueing %u ms", depth * 10);
        }
        if (queued >= int(depth) * kChunkBytes) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        render(mix.data(), kFrames);
        SDL_PutAudioStreamData(stream, mix.data(), kChunkBytes);
        if (queued + kChunkBytes >= int(depth) * kChunkBytes) primed = true;
    }
}

}  // namespace

void StartOutput(std::function<void(float* out, uint32_t frames)> render) {
    // the audio device's threads mix on the host and run no guest code: started off the guest threads' one CPU
    kernel::HostThreadsFree free_threads;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOGE("audio: SDL audio unavailable, no sound: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, int(kOutChannels), int(kRate)};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
        LOGE("audio: no audio device, no sound: %s", SDL_GetError());
        return;
    }
    std::thread(MixThread, stream, std::move(render)).detach();
    SDL_ResumeAudioStreamDevice(stream);
    LOGR("audio: SDL3 %u Hz stereo", kRate);
}

}  // namespace audio
