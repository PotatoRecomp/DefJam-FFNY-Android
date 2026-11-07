// Hooks for default.xbe. A strong definition replaces the recompiled function at that address, for example:
//
// #include "xdk/hle.h"
//
// X86_EXTERN_FUNC(__imp__sub_00012345);
// X86_FUNC(sub_00012345) {
//     __imp__sub_00012345(ctx, base);  // the original
// }
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

#include "common/options.h"
#include "core/log.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

// The created fighter's snapshot (profile portrait and thumbnail). The game renders the fighter into a small swizzled
// surface, then this function memcpys that surface's pixels (0x1794A) into the image it quantizes into the profile.
// It never locks the surface, so the pixels the GPU drew are written back to its memory here first; without that the
// saved portrait is empty. The source is what 0x00100FB0 returns for [this + 8]: [[[this + 8] + 0x40] + 0x18], an
// image whose pixels follow its header at +0x10, or lie [+0x10] bytes from it when flag 0x1000 of [+0x0C] is set.
X86_EXTERN_FUNC(__imp__sub_000178B0);
X86_FUNC(sub_000178B0) {
    using kernel::R32;
    uint32_t owner = R32(ctx.ecx + 0x08);
    uint32_t holder = owner ? R32(owner + 0x40) : 0;
    uint32_t image = holder ? R32(holder + 0x18) : 0;
    if (image) {
        constexpr uint32_t kExternalPixels = 0x1000;
        uint32_t pixels = R32(image + 0x0C) & kExternalPixels ? image + R32(image + 0x10) : image + 0x10;
        d3d::SyncSurface(pixels);
    }
    __imp__sub_000178B0(ctx, base);
}

// A resource library of the title (0x130FB0 to 0x133CC0) guards its state with a busy flag at 0x3AD938 and keeps the
// entry being served at 0x3AD928: each entry tests the flag, sets it, and clears both on the way out, returning
// 0xFFFFFFEE when it finds the flag set. On the console's one CPU that test and set is as good as atomic. Here guest
// threads run in parallel: two threads pass the test together, the first to leave clears 0x3AD928 under the other,
// which then reads through null (0x1328C6, [[0x3AD928] + 0x38]) and the game crashes at random, entering the gym or
// any screen that loads. The entries are serialized across threads; recursive, so a nested call on one thread still
// meets the flag and gets busy, as on the console.
// The lock is let go while its holder blocks in the kernel (kernel::Blocking): a holder waiting for a thread that
// itself calls into the library (the audio stream's reset, 0x133C00) would otherwise wait forever, where the
// console's one CPU ran that thread and had it find the flag set.
namespace {
class ResourceLock {
public:
    // A thread that cannot have the lock within kPatience goes on without it, so the lock can never freeze the game:
    // locks the runtime takes in between (the dispatch level's) could otherwise wait on each other for ever. Without
    // it the call meets the library's own busy flag and returns busy, as on the console. False: not taken.
    bool lock() {
        if (owner_.load() == std::this_thread::get_id()) {
            depth_++;
            return true;
        }
        if (!m_.try_lock_for(kPatience)) return false;
        take(1);
        return true;
    }
    void unlock() {
        if (owner_.load() != std::this_thread::get_id()) return;  // gave it up at a wait and could not have it back
        if (--depth_ == 0) give();
    }
    // the whole hold of this thread let go for a kernel wait, its depth returned; 0 when it holds nothing
    uint32_t release() {
        if (owner_.load() != std::this_thread::get_id()) return 0;
        uint32_t held = depth_;
        give();
        return held;
    }
    // after a wait: the hold it had, taken back if the lock is free within kPatience; else the rest of the call runs
    // without it, as on the console. A call that should not run without it (strict) waits longer, kStrictPatience:
    // every other holder lets it go at its own kernel waits, so a longer one is a holder stuck elsewhere, logged.
    void reacquire(uint32_t held, bool strict) {
        if (!m_.try_lock_for(strict ? kStrictPatience : kPatience)) {
            LOGW("stream lock: not back within %d ms after a wait, held by %s; retrying...",
                 int(std::chrono::duration_cast<std::chrono::milliseconds>(strict ? kStrictPatience : kPatience).count()),
                 Holder().c_str());
            if (!m_.try_lock_for(kStrictPatience)) {
                LOGE("stream lock: contention timed out, held by %s", Holder().c_str());
                return;
            }
        }
        take(held);
    }
    // the thread holding the lock as the log names it: its id and name, "nobody" when free
    std::string Holder() const {
        int tid = tid_.load();
        if (!tid) return "nobody";
        std::string name;
        std::ifstream(std::string("/proc/self/task/") + std::to_string(tid) + "/comm") >> name;
        return "thread " + std::to_string(tid) + " (" + (name.empty() ? "?" : name) + ")";
    }

private:
    void take(uint32_t depth) {
        owner_ = std::this_thread::get_id();
        tid_ = int(gettid());
        depth_ = depth;
    }
    void give() {
        depth_ = 0;
        tid_ = 0;
        owner_ = std::thread::id();
        m_.unlock();
    }

    static constexpr auto kPatience = std::chrono::milliseconds(100);
    static constexpr auto kStrictPatience = std::chrono::seconds(2);
    std::timed_mutex m_;
    std::atomic<std::thread::id> owner_{};
    std::atomic<int> tid_{0};  // the owner's native id, for the log
    uint32_t depth_ = 0;  // the owner's alone
};
ResourceLock g_resource_lock;
thread_local bool t_strict = false;  // this thread is in a call that never runs without the lock
}  // namespace

// for the freeze watchdog's report (native/main.cpp)
std::string game::ResourceLockHolder() { return g_resource_lock.Holder(); }

namespace {
const bool g_resource_lock_hooked = [] {
    kernel::g_block_hooks.release = [] { return g_resource_lock.release(); };
    kernel::g_block_hooks.reacquire = [](uint32_t held) { g_resource_lock.reacquire(held, t_strict); };
    return true;
}();
}  // namespace
#define SERIALIZED(addr)                                                      \
    X86_EXTERN_FUNC(__imp__sub_##addr);                                       \
    X86_FUNC(sub_##addr) {                                                    \
        const bool held = g_resource_lock.lock();                             \
        if (!held) {                                                          \
            LOGW("stream lock: SERIALIZED(" #addr ") lock contention, held by %s", g_resource_lock.Holder().c_str()); \
        }                                                                     \
        __imp__sub_##addr(ctx, base);                                         \
        if (held) g_resource_lock.unlock();                                   \
    }
SERIALIZED(00130FB0)
SERIALIZED(00131080)
SERIALIZED(00131310)
SERIALIZED(00131620)
SERIALIZED(001317C0)
SERIALIZED(00131A20)
SERIALIZED(00131A90)
SERIALIZED(00131B00)
SERIALIZED(00131BE0)
SERIALIZED(00131C00)
SERIALIZED(00131DB0)
SERIALIZED(00132260)
SERIALIZED(00132380)
SERIALIZED(00132440)
SERIALIZED(00132530)
SERIALIZED(00132670)
SERIALIZED(001329A0)
SERIALIZED(00132A80)
SERIALIZED(00132770)
// the library's other users of the flag and the entry: reached through pointers or from a reset, as able to clear
// 0x3AD928 under another thread as the entries above
SERIALIZED(00132B10)
SERIALIZED(00133240)
SERIALIZED(00133340)
SERIALIZED(00133440)
SERIALIZED(00133550)
SERIALIZED(00133C00)

// The data streams ("STRM", 0x1E7320 to 0x1E87B0): a ring of records one thread reads (0x1E7D80, from the stream
// manager's 0x13B380) while others fill, reset and wrap it (0x1E7C10, 0x1E8600). Only the byte counts are under the
// stream's own critical section; the read and wrap pointers and the record lists rely on the console's one CPU never
// running two of these at once. Run together, the reader can follow a record pointer in the middle of another
// thread's update and read guest address 4 (the force close on loading). Every function that checks the
// STRM tag is serialized
// with the library above, under the same lock.
SERIALIZED(001E7320)
SERIALIZED(001E7900)
SERIALIZED(001E7AE0)
SERIALIZED(001E7B50)
SERIALIZED(001E7B80)
SERIALIZED(001E7BC0)
SERIALIZED(001E7BF0)
SERIALIZED(001E7C10)
SERIALIZED(001E7CD0)
// The reader is the one that freezes: run without the lock it can follow a record pointer another thread is midway
// through and walk the ring for ever (the main thread stuck in 0x1E7DF6..0x1E7E18 with the music still playing, met
// most after the app comes back from the background, when the holder ran on a throttled core past kPatience). It never
// runs unlocked: without the lock it returns as it does over an empty queue (eax 0), and its caller, polling every
// tick, asks again on the next one; past kReaderPatience polls in a row it reads without it, as before, rather than
// stall the game, and the log names the thread that held it. A wait inside it (the stream's critical section) gives
// the lock up as any call does and waits longer than others to take it back (strict).
X86_EXTERN_FUNC(__imp__sub_001E7D80);
namespace {
std::atomic<uint32_t> g_reader_misses{0};  // polls in a row the reader went without the lock
constexpr uint32_t kReaderPatience = 30;   // about half a second of its caller's ticks
}  // namespace
X86_FUNC(sub_001E7D80) {
    if (!g_resource_lock.lock()) {
        uint32_t missed = ++g_reader_misses;
        if (missed == kReaderPatience || (missed % 120 == 0)) {
            LOGW("stream lock: reader waiting for %s (%u polls missed)", g_resource_lock.Holder().c_str(), missed);
        }
        ctx.eax = 0;
        ctx.esp += 4;  // its ret: cdecl, the caller pops the argument
        return;
    }
    if (g_reader_misses.exchange(0) >= kReaderPatience) LOGI("stream lock: the reader has it again");
    // its wait for the stream's critical section lets the lock go; it is taken back before the walk (strict)
    const bool outer = t_strict;
    t_strict = true;
    __imp__sub_001E7D80(ctx, base);
    t_strict = outer;
    g_resource_lock.unlock();
}
SERIALIZED(001E7E30)
SERIALIZED(001E7E50)
SERIALIZED(001E7E70)
SERIALIZED(001E7EA0)
SERIALIZED(001E7EC0)
SERIALIZED(001E7EE0)
SERIALIZED(001E7F60)
SERIALIZED(001E82F0)
SERIALIZED(001E8340)
SERIALIZED(001E83D0)
SERIALIZED(001E8600)
SERIALIZED(001E87B0)

// The buffer rings' deferred release queue: a count at 0x3A9680 and 8 byte entries from 0x3A9684 (a word, 1 for a
// stream record, the ring's index, then the record). 0x126150 and 0x1262B0 append (write the entry at the count, then
// inc the count); 0x126300 walks the entries, releases each through the ring's callback (0x11EC80, into the STRM
// functions above), and sets the count to 0. On the console's one CPU nothing appended in between; here an entry
// appended while the queue drains is lost or read half written, and an old one released twice: the release then reads
// a header through a freed record (signal 11 in 0x11EC80 at [record - 4], thread 0x1242D0, logged on a phone) or
// leaves the streams' rings broken. Under the same lock as the streams, so the release's calls into them nest in it
SERIALIZED(00126150)
SERIALIZED(001262B0)
SERIALIZED(00126300)
// 0x126314 and 0x12631D are the same drain loop entered in the middle: not direct calls but callbacks the ring holds
// and runs from its own thread (0x1242D0), so neither took the lock 0x126300 does. Walking the entries unlocked, the
// loop reads the ring index [esi-0x02] of an entry another thread is appending, indexes the ring table at 0x3A9984
// with it and calls the release through [eax+0x58]: a half-written index gives a bogus table slot and the call lands
// on garbage (signal 11 on a wild target, thread 0x1242D0, logged on a phone). Serialized with the queue above, under
// the same lock.
SERIALIZED(00126314)
SERIALIZED(0012631D)

// Widescreen, as Warriors-Android does it: the 3D view is widened where the game builds it, so projection and culling
// grow with the screen. sub_00103E10 is View::SetPerspective(fov, aspect, near, far): fov the horizontal angle in
// degrees, m00 = cot(fov / 2), m11 = cot(fov / 2) * aspect, and the culling planes rebuilt from the same two
// (DefJamFFNY-recomp's widescreen-game-projection.md). Only the scene viewport's own calls, from its creation
// (0x1A6EE) and its refresh (0x1A792), are widened: Hor+, tan(fov / 2) times the screen's widening and the aspect with
// it, so the vertical view stays as authored. The render-to-texture views (the fighter snapshot) keep theirs, and the
// calls that put back stored values (after a 2D overlay, a viewport change) pass them on already widened.
X86_EXTERN_FUNC(__imp__sub_00103E10);
X86_FUNC(sub_00103E10) {
    using kernel::R32;
    const uint32_t ret = R32(ctx.esp);
    const float wide = game::g_wide;
    if (wide > 1 && (ret == 0x1A6EE || ret == 0x1A792)) {
        float& fov = kernel::G<float>(ctx.esp + 4);
        float& aspect = kernel::G<float>(ctx.esp + 8);
        constexpr float kDegToRad = 3.14159265358979f / 180.0f;
        const float half = std::atan(std::tan(fov * 0.5f * kDegToRad) * wide);
        if (half > 0 && half < 1.5f) {  // under 172 degrees across: a wider screen than that keeps the title's view
            fov = 2.0f * half / kDegToRad;
            aspect *= wide;
        }
    }
    __imp__sub_00103E10(ctx, base);
}

// The front end's song notification. feloader.apt, its persistent root movie, sets up the `chyron` clip with
// `chyron._x = 0; chyron._y = 385` and parks it at the lower left between songs with its torn-paper tab on screen;
// every launch reuses the stored y. Stretched across a phone that tab sits at the screen's edge for the whole menu, so
// the y is moved below the screen, taking the tab, the card and its slide with it. The literal is a PushShort operand
// patched after the loader resolves the movie's name (sub_001810B0) and before the data runs (sub_00056F50), and only
// when the instruction window hashes as the known build (ffny-online's widescreen.c, issue #118).
namespace {
constexpr uint32_t kChyronYAt = 0xCD1F, kChyronWindowAt = 0xCD10, kChyronWindowLen = 18, kChyronFnv = 0xB6C9DEB3;
constexpr int kChyronY = 385, kChyronHiddenY = 385 + 1000;
std::atomic<bool> g_feloader{false};

bool IsFeloader(uint32_t s) {
    uint32_t base = 0, i = 0;
    for (; i < 256 && kernel::G<uint8_t>(s + i); i++)
        if (kernel::G<uint8_t>(s + i) == '/' || kernel::G<uint8_t>(s + i) == '\\') base = i + 1;
    const char* want = "feloader";
    for (i = 0; want[i]; i++) {
        uint8_t c = kernel::G<uint8_t>(s + base + i);
        if (c >= 'A' && c <= 'Z') c = uint8_t(c - 'A' + 'a');
        if (c != uint8_t(want[i])) return false;
    }
    return kernel::G<uint8_t>(s + base + i) == 0;
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_001810B0);
X86_FUNC(sub_001810B0) {
    const uint32_t name = kernel::R32(ctx.esp + 8);
    g_feloader = game::Opt().aspect && name && IsFeloader(name);
    __imp__sub_001810B0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00056F50);
X86_FUNC(sub_00056F50) {
    const uint32_t apt = kernel::R32(ctx.esp + 8);
    if (g_feloader.exchange(false) && apt && !std::memcmp(&kernel::G<uint8_t>(apt), "Apt Data", 8)) {
        uint8_t& lo = kernel::G<uint8_t>(apt + kChyronYAt);
        uint8_t& hi = kernel::G<uint8_t>(apt + kChyronYAt + 1);
        uint32_t h = 0x811C9DC5u;
        for (uint32_t i = 0; i < kChyronWindowLen; i++) {
            const uint32_t at = kChyronWindowAt + i;
            const uint8_t b = at == kChyronYAt ? uint8_t(kChyronY & 0xFF)
                            : at == kChyronYAt + 1 ? uint8_t(kChyronY >> 8) : kernel::G<uint8_t>(apt + at);
            h = (h ^ b) * 0x01000193u;
        }
        if (h == kChyronFnv && int16_t(lo | hi << 8) == kChyronY) {
            lo = uint8_t(kChyronHiddenY & 0xFF);
            hi = uint8_t(kChyronHiddenY >> 8);
        }
    }
    __imp__sub_00056F50(ctx, base);
}

// The fight's clock and fighters freezing while the picture and the music go on. The game's logic thread (0xD7700)
// waits on an event that the D3D timer callback 0xD7EA0 sets; that callback re-arms itself through 0xD7DE0, a
// drift-corrected period: next = now + (period - lateness of this tick). 0x216410, the library's timer arm, refuses a
// deadline already within 40 cycles of the time stamp counter (returns 0x8876082A and arms nothing), and 0xD7DE0
// ignores the result. On the console the timer interrupt is never a whole period late; on a phone the miniport timer
// thread can be held off the CPU longer than that under load, the deadline comes out in the past, the arm is refused,
// and the tick never comes again. A deadline that has passed is armed to fire on the timer's next poll instead, as a
// late interrupt would have: 0xD7DE0 then sees the short intervals and catches its period up in a few ticks.
X86_EXTERN_FUNC(__imp__sub_00216410);
X86_FUNC(sub_00216410) {
    constexpr uint32_t kRefused = 0x8876082A;
    constexpr uint64_t kSoon = 733333333ull / 4000;  // 0.25 ms of 733.33 MHz counter: the miniport timer's poll
    // stdcall (deadline low, deadline high, callback, context); a null callback disarms and takes no deadline
    const uint32_t args = ctx.esp + 4, esp = ctx.esp;
    const bool arming = kernel::R32(args + 8) != 0;
    for (int tries = 0;; tries++) {
        if (arming) {
            uint64_t deadline = kernel::R32(args) | uint64_t(kernel::R32(args + 4)) << 32;
            uint64_t soonest = x86_rdtsc() + kSoon;
            if (deadline < soonest) {
                kernel::W32(args, uint32_t(soonest));
                kernel::W32(args + 4, uint32_t(soonest >> 32));
            }
        }
        __imp__sub_00216410(ctx, base);
        // held off the CPU between the clamp and the library's own read of the counter: arm again, a little later
        if (!arming || ctx.eax != kRefused || tries == 3) break;
        ctx.esp = esp;  // the refusal popped the arguments with ret 0x10; they are still in place below
    }
}

// Venue post-effects: color filter, vignette quad, and corner darkening quads (sub_000D8C40 / sub_000D9AF0).
// When disable_venue_filter is true, skips drawing the platform tint, vignette, and corner darkening quads,
// removing the murky dark green overlay and dark corners from venues.
X86_EXTERN_FUNC(__imp__sub_000D8C40);
X86_FUNC(sub_000D8C40) {
    if (game::Opt().disable_venue_filter) {
        ctx.esp += 4;  // callee pops return address
        return;
    }
    __imp__sub_000D8C40(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_000D9AF0);
X86_FUNC(sub_000D9AF0) {
    if (game::Opt().disable_venue_filter) {
        ctx.esp += 32;  // pop return address (4) + 7 dword arguments (28)
        return;
    }
    __imp__sub_000D9AF0(ctx, base);
}
