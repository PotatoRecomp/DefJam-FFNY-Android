// Cheats the game has no codes for, switched in the settings panel's CHEATS tab and kept in the settings file.
//
// Unlock all: the retail image still carries the developers' unlock-everything routine, sub_000158F0, whose only
// caller is a branch of the Extras code handler the code classifier never selects (ffny-online's
// docs/research/unlock-everything.md). It fetches the game-data object (slot 0x18 of the managers at 0x2FB148: the
// profile at +4, the data tables at +8), inserts every character (table key 0x4CE5B1B2) and venue (0xD7DCA628) into
// the profile's unlocked sets and sets the reward points (profile +0x50) to 10,000. It changes the profile in memory
// only, so it is run again on every screen the front end opens (Control.GetScreenInfo, sub_00055490) once the main
// menu has started (Game.StartMainMenu, sub_00046610): a profile loaded later is unlocked before a screen reads it.
//
// Perfect parry: fighter records are 0x12E8 bytes from 0x3B92E0, one a slot, +0x30 the state (the handlers' table
// built in sub_001B54D0), +0x34 its phase, +0x937 bit 0 set for a CPU fighter, +0x04 and +0x08 the partners.
// - A strike reaches the defender through sub_001C6CD0(attacker, defender, hit flags, 0). A defender in the counter
//   stance (state 0x4E) inside the "block counter frame" window of gamecore.xml has sub_001D8B40 pair the two in a
//   counter: attacker states 0x45, 0x46, 0x48 become 0x4F (defender) and 0x50, states 0x6C to 0x6E become 0x53 and
//   0x54. A defender holding block (state 2, phase 2) takes the hit as a guarded one (state 3) instead.
// - A grab reaches its victim through the catch at 0x1D66A0, which asks sub_001D8BD0(victim, grabber) whether the
//   victim counters it: in the stance and inside the window, the victim is put in state 0x51, its counter strike,
//   and the grab misses. Grabs of class 6 (attack ids 0x71 to 0x73) are never asked, as on the console.
// With the cheat on a human fighter takes every such hit or grab as if the timing had been perfect: the game's own
// counter for a strike it can counter, its guard for one it cannot, and the reversal for a grab. A defender who is
// down, held, pinned or already in a paired move is left to the game.
#include <jni.h>

#include <cstdint>

#include "app/app.h"
#include "common/options.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace game {

namespace {

enum Cheat : int { kUnlockAll, kUnlockStoryItems, kStoryDevPts, kStoryCash, kPerfectParry, kCheatCount };
static_assert(kCheatCount == sizeof(Options::cheats) / sizeof(bool));

bool On(Cheat c) { return Opt().cheats[c]; }

// the front end
constexpr uint32_t kManagers = 0x2FB148, kGameData = 0x18;
constexpr uint32_t kUnlockEverything = 0x158F0;
bool g_main_menu = false;  // Game.StartMainMenu has run: the managers the routine reads are up

// Story character slots: 16 slots of 0x5950 bytes starting at profile + 0x177C.
// Active story slot index is at profile + 0x5AC7C.
// Money / cash is at offset +0x20 within the character record (profile + slot * 0x5950 + 0x179C).
// Dev points are at offset +0x28 within the character record (profile + slot * 0x5950 + 0x17A4).
constexpr uint32_t kStoryRecordBase = 0x177C, kStoryRecordSize = 0x5950, kStorySlots = 16;
constexpr uint32_t kActiveStorySlot = 0x5AC7C;
constexpr uint32_t kStoryCashOffset = 0x20, kStoryDevPtsOffset = 0x28;
constexpr uint32_t kInfiniteStoryPointsVal = 999999;

// Shop unlock item containers in profile (each 0x58 bytes):
// 0x14E4: Barber (hairstyles, facial hair, hair colors)
// 0x153C: Jewelry (chains, pendants, watches, rings, earrings)
// 0x1594: Clothes (shirts, pants, shoes, hats, etc.)
// 0x15EC: Tattoos (arms, legs, back, chest, etc.)
// 0x1644: Moves (fighting styles, blazing moves)
// 0x169C: Voices / Bling
constexpr uint32_t kShopContainers[] = {0x14E4, 0x153C, 0x1594, 0x15EC, 0x1644, 0x169C};
constexpr uint32_t kDebugFlags = 0x2FB1F0;

void UnlockContainer(uint32_t container, uint8_t val, uint32_t start_idx = 0) {
    if (!container) return;
    uint32_t count = kernel::R32(container + 0x08);
    if (!count || count > 5000) return;
    uint32_t ptr = kernel::R32(container + 0x4C);
    if (!ptr) return;
    uint32_t buf_idx = kernel::R32(ptr);
    if (buf_idx > 15) buf_idx = 0;
    uint32_t buf = kernel::R32(container + buf_idx * 4 + 0x0C);
    if (!buf) return;
    uint8_t mask = kernel::G<uint8_t>(container + 0x50);
    if (!mask) mask = 3;

    for (uint32_t i = start_idx; i < count; i++) {
        uint32_t byte_idx = i >> 2;
        uint32_t shift = (i & 3) << 1;
        uint8_t cur = kernel::G<uint8_t>(buf + byte_idx);
        cur &= ~(mask << shift);
        cur |= (val & mask) << shift;
        kernel::G<uint8_t>(buf + byte_idx) = cur;
    }
}

void RepairStoryStyles(uint32_t data = 0) {
    if (!data) data = kernel::R32(kManagers + kGameData * 4);
    if (!data) return;
    uint32_t profile = kernel::R32(data + 4);
    if (!profile) return;

    uint32_t container = profile + 0x1644;
    uint32_t count = kernel::R32(container + 0x08);
    if (!count) return;
    uint32_t ptr = kernel::R32(container + 0x4C);
    if (!ptr) return;
    uint32_t buf_idx = kernel::R32(ptr);
    if (buf_idx > 15) buf_idx = 0;
    uint32_t buf = kernel::R32(container + buf_idx * 4 + 0x0C);
    if (!buf) return;

    uint32_t active = kernel::R32(profile + kActiveStorySlot);
    if (active >= kStorySlots) return;
    uint32_t rec = profile + kStoryRecordBase + active * kStoryRecordSize;

    if (kernel::G<uint8_t>(rec + 0x08) == 0 && kernel::G<uint8_t>(rec + 0x09) == 0 &&
        kernel::R32(rec + kStoryCashOffset) == 0 && kernel::R32(rec + kStoryDevPtsOffset) == 0) {
        return;
    }

    uint8_t s0 = kernel::G<uint8_t>(rec + 0x140);
    uint8_t s1 = kernel::G<uint8_t>(rec + 0x141);
    uint8_t s2 = kernel::G<uint8_t>(rec + 0x142);

    // Fighting styles 0..4 are stored in the first 10 bits of buf (2 bits each: 0=unlearned, 2=learned)
    uint16_t cur = kernel::G<uint16_t>(buf);
    cur &= ~0x03FF;
    if (s0 < 5) cur |= uint16_t(2 << (s0 * 2));
    if (s1 < 5) cur |= uint16_t(2 << (s1 * 2));
    if (s2 < 5) cur |= uint16_t(2 << (s2 * 2));
    kernel::G<uint16_t>(buf) = cur;
}

void RepairStoryBarber(uint32_t data = 0) {
    if (!data) data = kernel::R32(kManagers + kGameData * 4);
    if (!data) return;
    uint32_t profile = kernel::R32(data + 4);
    if (!profile) return;

    uint32_t container = profile + 0x14E4;
    uint32_t count = kernel::R32(container + 0x08);
    if (!count || count > 5000) return;
    uint32_t ptr = kernel::R32(container + 0x4C);
    if (!ptr) return;
    uint32_t buf_idx = kernel::R32(ptr);
    if (buf_idx > 15) buf_idx = 0;
    uint32_t buf = kernel::R32(container + buf_idx * 4 + 0x0C);
    if (!buf) return;

    // In normal gameplay at most 2 items have status 3 (equipped hair & beard).
    // If more than 5 items have status 3, the container was corrupted by previous cheat logic.
    uint32_t threes = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t val = (kernel::G<uint8_t>(buf + (i >> 2)) >> ((i & 3) << 1)) & 3;
        if (val == 3) threes++;
    }
    if (threes > 5) {
        UnlockContainer(container, 1);
    }
}

void ApplyStoryUnlocks(uint32_t data = 0) {
    if (!data) data = kernel::R32(kManagers + kGameData * 4);
    if (!data) return;
    uint32_t profile = kernel::R32(data + 4);
    if (!profile) return;

    // Barber container 0x14E4: unlock with 1 (available in shop), matching official sub_00016040
    UnlockContainer(profile + 0x14E4, 1);

    // Jewelry 0x153C, Clothes 0x1594, Tattoos 0x15EC, Voices 0x169C: unlock with 3 (owned)
    UnlockContainer(profile + 0x153C, 3);
    UnlockContainer(profile + 0x1594, 3);
    UnlockContainer(profile + 0x15EC, 3);
    UnlockContainer(profile + 0x169C, 3);

    // Moves container 0x1644: only unlock blazing moves (index >= 5) with 3; styles 0..4 left to player
    UnlockContainer(profile + 0x1644, 3, 5);

    // Re-assert proper learned style bits for active character
    RepairStoryStyles(data);

    kernel::W32(kDebugFlags, kernel::R32(kDebugFlags) | 0x05);
}

void ApplyStoryCash(uint32_t data = 0) {
    if (!data) data = kernel::R32(kManagers + kGameData * 4);
    if (!data) return;
    uint32_t profile = kernel::R32(data + 4);
    if (!profile) return;

    uint32_t active = kernel::R32(profile + kActiveStorySlot);
    if (active < kStorySlots) {
        uint32_t rec = profile + kStoryRecordBase + active * kStoryRecordSize;
        kernel::W32(rec + kStoryCashOffset, kInfiniteStoryPointsVal);
    }
    for (uint32_t i = 0; i < kStorySlots; i++) {
        uint32_t rec = profile + kStoryRecordBase + i * kStoryRecordSize;
        if (kernel::G<uint8_t>(rec + 0x08) != 0 || kernel::G<uint8_t>(rec + 0x09) != 0 ||
            kernel::R32(rec + kStoryCashOffset) != 0 || kernel::R32(rec + kStoryDevPtsOffset) != 0) {
            kernel::W32(rec + kStoryCashOffset, kInfiniteStoryPointsVal);
        }
    }
}

void ApplyStoryDevPts(uint32_t data = 0) {
    if (!data) data = kernel::R32(kManagers + kGameData * 4);
    if (!data) return;
    uint32_t profile = kernel::R32(data + 4);
    if (!profile) return;

    uint32_t active = kernel::R32(profile + kActiveStorySlot);
    if (active < kStorySlots) {
        uint32_t rec = profile + kStoryRecordBase + active * kStoryRecordSize;
        kernel::W32(rec + kStoryDevPtsOffset, kInfiniteStoryPointsVal);
    }
    for (uint32_t i = 0; i < kStorySlots; i++) {
        uint32_t rec = profile + kStoryRecordBase + i * kStoryRecordSize;
        if (kernel::G<uint8_t>(rec + 0x08) != 0 || kernel::G<uint8_t>(rec + 0x09) != 0 ||
            kernel::R32(rec + kStoryCashOffset) != 0 || kernel::R32(rec + kStoryDevPtsOffset) != 0) {
            kernel::W32(rec + kStoryDevPtsOffset, kInfiniteStoryPointsVal);
        }
    }
}

// the fighters
constexpr uint32_t kFighters = 0x3B92E0, kFighterSize = 0x12E8, kSlots = 4;
constexpr uint32_t kPartner1 = 0x04, kPartner2 = 0x08, kLastAttacker = 0x0C;
constexpr uint32_t kState = 0x30, kFlags91E = 0x91E, kControl = 0x937;
constexpr uint32_t kHitFlags = 0x914, kBearing = 0x918, kBearingExtra = 0xFE0;
constexpr uint32_t kAttackInfo = 0xF50, kAttackInfoTaken = 0xFB8, kAttackInfoWords = 0x1A;
constexpr uint32_t kHitResult = 0x41C;  // the attacker's: 4 landed, 0x10 guarded, 0x20 countered
constexpr uint32_t kHitFlagNoFighter = 0x400;  // a hit the game gives no attacker relation to
constexpr uint16_t kUntouchable = 0x0E00, kPaired = 0x0001;  // +0x91E: sub_001C6CD0 ignores the hit; in a pair move

constexpr uint32_t kSetBearing = 0x1C5E90;  // (attacker, defender, 1)
constexpr uint32_t kCounterPair = 0x1D8B40;  // (defender, attacker): 1 when the strike is one it counters
constexpr uint32_t kSetState = 0x1B3CA0;     // (fighter, state)
constexpr uint32_t kFighterEvent = 0x1A6870; // (fighter, event)
constexpr uint32_t kGuardedHit = 3, kGrabCounter = 0x51, kCounterEvent = 0x1B;

bool IsFighter(uint32_t f) { return f >= kFighters && f < kFighters + kSlots * kFighterSize && (f - kFighters) % kFighterSize == 0; }

bool IsHuman(uint32_t f) { return IsFighter(f) && !(kernel::G<uint8_t>(f + kControl) & 1); }

bool Partners(uint32_t a, uint32_t b) { return kernel::R32(a + kPartner1) == b || kernel::R32(a + kPartner2) == b; }

// standing, guarding, reeling from a hit or striking: where a parry reads as one
bool CanParry(uint32_t f) {
    uint32_t s = kernel::R32(f + kState);
    bool standing = s <= 5 || (s >= 0x0E && s <= 0x12) || (s >= 0x45 && s <= 0x4E) || (s >= 0x6C && s <= 0x70);
    return standing && !(kernel::R16(f + kFlags91E) & (kUntouchable | kPaired));
}

// attacker states sub_001D8B40 pairs with a counter
bool Counterable(uint32_t attacker) {
    uint32_t s = kernel::R32(attacker + kState);
    return s == 0x45 || s == 0x46 || s == 0x48 || (s >= 0x6C && s <= 0x6E);
}

}  // namespace

}  // namespace game

// Game.StartMainMenu
X86_EXTERN_FUNC(__imp__sub_00046610);
X86_FUNC(sub_00046610) {
    __imp__sub_00046610(ctx, base);
    game::g_main_menu = true;
}

// Control.GetScreenInfo: each screen the front end opens, the unlock run again after it
X86_EXTERN_FUNC(__imp__sub_00055490);
X86_FUNC(sub_00055490) {
    __imp__sub_00055490(ctx, base);
    using namespace game;
    if (!g_main_menu) return;
    uint32_t data = kernel::R32(kManagers + kGameData * 4);
    if (!data || !kernel::R32(data + 4)) return;
    if (On(kUnlockAll) && kernel::R32(data + 8)) {
        kernel::CallGuest(ctx, kUnlockEverything, {});
    }
    if (On(kStoryCash)) {
        ApplyStoryCash(data);
    }
    if (On(kStoryDevPts)) {
        ApplyStoryDevPts(data);
    }
    if (On(kUnlockStoryItems)) {
        ApplyStoryUnlocks(data);
    }
    RepairStoryStyles(data);
    RepairStoryBarber(data);
}

// ItemMgr::GetItemUnlockState: query item unlock status (0: locked, 1: shop, 2: bought, 3: owned)
X86_EXTERN_FUNC(__imp__sub_00017260);
X86_FUNC(sub_00017260) {
    uint32_t container = ctx.ecx;  // Save container pointer before __imp__sub_00017260 modifies ctx.ecx
    __imp__sub_00017260(ctx, base);
    if (game::On(game::kUnlockStoryItems) && ctx.eax != 0xFFFFFFFFu) {
        uint32_t data = kernel::R32(game::kManagers + game::kGameData * 4);
        uint32_t profile = data ? kernel::R32(data + 4) : 0;
        // Do NOT override Moves container (0x1644) or Barber container (0x14E4)
        if (!profile || (container != profile + 0x1644 && container != profile + 0x14E4)) {
            ctx.eax = 3;
        }
    }
}

// ChooseShopList
X86_EXTERN_FUNC(__imp__sub_0004C6C0);
X86_FUNC(sub_0004C6C0) {
    using namespace game;
    RepairStoryStyles();
    RepairStoryBarber();
    if (On(kUnlockStoryItems)) {
        ApplyStoryUnlocks();
    }
    __imp__sub_0004C6C0(ctx, base);
}

// ResetItemList
X86_EXTERN_FUNC(__imp__sub_0004C990);
X86_FUNC(sub_0004C990) {
    using namespace game;
    RepairStoryStyles();
    RepairStoryBarber();
    if (On(kUnlockStoryItems)) {
        ApplyStoryUnlocks();
    }
    __imp__sub_0004C990(ctx, base);
}

// GetPlayerCash: shop and crib cash readout
X86_EXTERN_FUNC(__imp__sub_0004CA30);
X86_FUNC(sub_0004CA30) {
    using namespace game;
    if (On(kStoryCash)) {
        ApplyStoryCash();
    }
    RepairStoryStyles();
    RepairStoryBarber();
    if (On(kUnlockStoryItems)) {
        ApplyStoryUnlocks();
    }
    __imp__sub_0004CA30(ctx, base);
}

// GetPlayerDevPts: gym and front-end dev points readout
X86_EXTERN_FUNC(__imp__sub_0004CA70);
X86_FUNC(sub_0004CA70) {
    using namespace game;
    if (On(kStoryDevPts)) {
        ApplyStoryDevPts();
    }
    RepairStoryStyles();
    RepairStoryBarber();
    if (On(kUnlockStoryItems)) {
        ApplyStoryUnlocks();
    }
    __imp__sub_0004CA70(ctx, base);
}

// a strike on a fighter: what the hit function does before it looks at the defender's state, then the counter or the
// guard (the success paths of its cases for states 0x4E and 2), with no timing window and from any side
X86_EXTERN_FUNC(__imp__sub_001C6CD0);
X86_FUNC(sub_001C6CD0) {
    using namespace game;
    using kernel::R32;
    const uint32_t attacker = R32(ctx.esp + 4), defender = R32(ctx.esp + 8), flags = R32(ctx.esp + 12);
    if (!On(kPerfectParry) || !IsHuman(defender) || !IsFighter(attacker) || attacker == defender ||
        (flags & kHitFlagNoFighter) || Partners(defender, attacker) || !CanParry(defender)) {
        __imp__sub_001C6CD0(ctx, base);
        return;
    }
    kernel::G<uint8_t>(attacker + kHitResult) |= 4;
    kernel::W32(defender + kLastAttacker, attacker);
    if (!Partners(attacker, defender)) kernel::W32(defender, attacker);
    kernel::W32(defender + kHitFlags, flags);
    kernel::W16(defender + kBearing, 0);
    for (uint32_t i = 0; i < kAttackInfoWords; i++)
        kernel::W32(defender + kAttackInfoTaken + i * 4, R32(attacker + kAttackInfo + i * 4));
    kernel::CallGuest(ctx, kSetBearing, {attacker, defender, 1});
    kernel::W16(defender + kBearing,
                uint16_t(kernel::R16(defender + kBearing) | int16_t(kernel::G<int8_t>(defender + kBearingExtra))));

    if (Counterable(attacker) && kernel::CallGuest(ctx, kCounterPair, {defender, attacker})) {
        kernel::G<uint8_t>(attacker + kHitResult) |= 0x20;
        kernel::CallGuest(ctx, kFighterEvent, {defender, kCounterEvent});
    } else {
        kernel::CallGuest(ctx, kSetState, {defender, kGuardedHit});
        kernel::G<uint8_t>(attacker + kHitResult) |= 0x10;
    }
    ctx.esp += 4;  // its ret: cdecl, the caller pops the arguments
}

// a grab catching a fighter: the reversal, as the counter stance gives it inside its window
X86_EXTERN_FUNC(__imp__sub_001D8BD0);
X86_FUNC(sub_001D8BD0) {
    using namespace game;
    using kernel::R32;
    const uint32_t victim = R32(ctx.esp + 4), grabber = R32(ctx.esp + 8);
    if (!On(kPerfectParry) || !IsHuman(victim) || !IsFighter(grabber) || victim == grabber ||
        Partners(victim, grabber) || !CanParry(victim)) {
        __imp__sub_001D8BD0(ctx, base);
        return;
    }
    kernel::W32(victim, grabber);
    kernel::W32(grabber + kLastAttacker, victim);
    kernel::W32(grabber, victim);
    kernel::CallGuest(ctx, kSetState, {victim, kGrabCounter});
    ctx.eax = 1;
    ctx.esp += 4;
}

extern "C" JNIEXPORT jbooleanArray JNICALL Java_com_datfooldive_defjam_DefJamActivity_nativeCheats(JNIEnv* env, jclass) {
    jboolean v[game::kCheatCount];
    for (int i = 0; i < game::kCheatCount; i++) v[i] = game::Opt().cheats[i];
    jbooleanArray arr = env->NewBooleanArray(game::kCheatCount);
    if (arr) env->SetBooleanArrayRegion(arr, 0, game::kCheatCount, v);
    return arr;
}

extern "C" JNIEXPORT void JNICALL Java_com_datfooldive_defjam_DefJamActivity_nativeCheatSet(JNIEnv*, jclass, jint i,
                                                                                         jboolean on) {
    if (i < 0 || i >= game::kCheatCount) return;
    game::Opt().cheats[i] = on;
    if (i == game::kStoryCash && on && game::g_main_menu) {
        game::ApplyStoryCash();
    }
    if (i == game::kStoryDevPts && on && game::g_main_menu) {
        game::ApplyStoryDevPts();
    }
    if (i == game::kUnlockStoryItems && on && game::g_main_menu) {
        game::ApplyStoryUnlocks();
    }
    if (game::g_main_menu) {
        game::RepairStoryStyles();
        game::RepairStoryBarber();
    }
    app::Save();
}
