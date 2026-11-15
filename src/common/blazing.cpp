#include "common/blazing.h"
#include "kernel/kernel.h"

namespace game {

// Retail default.xbe: sub_001A55C0 changes fighter +0x93E from ready (1) to blazing (2).
// sub_001C9DF0 starts the appeal from a new right-stick push. The grab handlers
// sub_001D3800 (0x1F) and sub_001D41D0 (0x2E) select moves with the same stick.
void BlazingPad(int port, input::Pad& p) {
    if (port < 0 || port >= 4 || !p.connected || p.analog[6] < 128 || p.analog[7] < 128 || !kernel::g_base)
        return;
    using kernel::R32;
    const uint32_t flags = R32(0x3B9054);
    if (R32(0x3B8F30) != 1 || ((flags & 0x60) && !(flags & 0x80))) return;

    constexpr uint32_t fighters = 0x3B92E0, stride = 0x12E8;
    for (uint32_t slot = 0; slot < 4; ++slot) {
        const uint32_t f = fighters + slot * stride;
        if (R32(f + 0x28) != slot || R32(0x3B9098 + slot * 4) != uint32_t(port) ||
            (kernel::G<uint8_t>(f + 0x937) & 1)) continue;
        const uint8_t blazing = kernel::G<uint8_t>(f + 0x93E);
        const uint32_t state = R32(f + 0x30), phase = R32(f + 0x34);
        const bool ready = (blazing & 1) && !(blazing & 2) && (state == 1 || state == 2 || state == 0x55);
        const uint32_t target = R32(f);
        const bool fighter = target >= fighters && target < fighters + stride * 4 && (target - fighters) % stride == 0;
        const bool grabbing = (blazing & 2) && fighter && target != f && R32(target) == f &&
            ((state == 0x2E && R32(target + 0x30) == 0x2F) || (state == 0x1F && R32(target + 0x30) == 0x20));
        if (!ready && !grabbing) return;

        const uint16_t direction = p.buttons & 0x0F;
        p.buttons &= ~uint16_t(0x0F);
        p.analog[6] = p.analog[7] = 0;
        p.rx = p.ry = 0;
        // Releasing block first lets state 2 return to standing; only standing starts an appeal.
        if (ready) {
            if (state == 1) p.ry = 32767;
            return;
        }
        // Wait for the game's selection window so an early held direction still produces a fresh stick edge.
        const bool choosing = state == 0x2E ? ((phase == 2 || phase == 7) && (kernel::R16(f + 0x920) & 1))
                                           : (phase == 3);
        if (!choosing) return;
        switch (direction) {
            case 1: p.ry = 32767; break;
            case 2: p.ry = -32767; break;
            case 4: p.rx = -32767; break;
            case 8: p.rx = 32767; break;
            default: break;  // neutral for diagonal or opposing directions: never select an arbitrary move
        }
        return;
    }
}

}  // namespace game
