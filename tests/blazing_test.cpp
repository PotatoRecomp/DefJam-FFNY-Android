// Run: c++ -std=c++20 -Isrc -Isdk/runtime tests/blazing_test.cpp src/common/blazing.cpp -o /tmp/defjam-blazing-test && /tmp/defjam-blazing-test
#include <cassert>
#include <cstring>
#include <vector>

#include "common/blazing.h"
#include "kernel/kernel.h"

namespace kernel { uint8_t* g_base = nullptr; }

namespace {
constexpr uint32_t f = 0x3B92E0, opponent = f + 0x12E8;
input::Pad Chord(uint16_t direction = 0) {
    input::Pad p;
    p.connected = true;
    p.analog[6] = p.analog[7] = 255;
    p.buttons = direction | 0x10;
    p.lx = 1234;
    p.analog[0] = 255;
    return p;
}
void Unchanged(int port = 0) {
    auto p = Chord(1), expected = p;
    game::BlazingPad(port, p);
    assert(p.buttons == expected.buttons && p.analog[6] == 255 && p.analog[7] == 255 && p.rx == 0 && p.ry == 0);
}
void Fighter(uint32_t state, uint8_t blazing, uint32_t phase = 0) {
    kernel::W32(f + 0x30, state);
    kernel::W32(f + 0x34, phase);
    kernel::G<uint8_t>(f + 0x93E) = blazing;
}
void Consumed(const input::Pad& p) {
    assert(p.analog[6] == 0 && p.analog[7] == 0 && p.buttons == 0x10);
    assert(p.lx == 1234 && p.analog[0] == 255);
}
}

int main() {
    Unchanged(); // safe before guest memory exists
    std::vector<uint8_t> memory(0x400000);
    kernel::g_base = memory.data();
    kernel::W32(f + 0x28, 0);
    kernel::W32(opponent + 0x28, 1);
    kernel::W32(0x3B9098, 0);
    for (int i = 1; i < 4; ++i) kernel::W32(0x3B9098 + i * 4, -1);
    Fighter(1, 1);
    Unchanged(); // outside a fight
    kernel::W32(0x3B8F30, 1);
    auto p = Chord();
    game::BlazingPad(0, p);
    Consumed(p);
    assert(p.rx == 0 && p.ry == 32767);
    // Held activation stays held across host polls: the game creates the single press edge.
    p = Chord(); game::BlazingPad(0, p); assert(p.ry == 32767);
    Fighter(0x55, 1); p = Chord(); game::BlazingPad(0, p); Consumed(p); assert(p.ry == 0);
    Fighter(2, 1); p = Chord(); game::BlazingPad(0, p); Consumed(p); assert(p.ry == 0);
    Fighter(1, 0); Unchanged(); // no meter
    Fighter(1, 2); Unchanged(); // blazing without a grab
    Fighter(0x35, 2); Unchanged(); // being held is not holding an opponent
    Fighter(1, 1);
    for (int side = 6; side <= 7; ++side) {
        p = Chord(); p.analog[side] = 127;
        game::BlazingPad(0, p); assert(p.analog[6] || p.analog[7]); assert(p.ry == 0);
    }
    p = Chord(); p.connected = false; game::BlazingPad(0, p); assert(p.ry == 0);
    kernel::W32(0x3B9054, 0x20); Unchanged(); kernel::W32(0x3B9054, 0);
    kernel::G<uint8_t>(f + 0x937) = 1; Unchanged(); kernel::G<uint8_t>(f + 0x937) = 0;
    Unchanged(-1); Unchanged(4); Unchanged(1);
    // Port lookup follows the fighter assignment, not fighter slot zero.
    kernel::W32(0x3B9098, 3);
    Unchanged(0); p = Chord(); game::BlazingPad(3, p); assert(p.ry == 32767);
    kernel::W32(0x3B9098, 0);
    Fighter(0x2E, 2, 2);
    kernel::W32(f, opponent); kernel::W32(opponent, f); kernel::W32(opponent + 0x30, 0x2F);
    kernel::W16(f + 0x920, 1);
    const int16_t x[] = {0, 0, -32767, 32767}, y[] = {32767, -32767, 0, 0};
    for (int i = 0; i < 4; ++i) {
        p = Chord(1 << i); game::BlazingPad(0, p); Consumed(p);
        assert(p.rx == x[i] && p.ry == y[i]);
    }
    for (int direction : {0, 3, 5, 6, 9, 10, 12, 15}) {
        p = Chord(direction); game::BlazingPad(0, p); Consumed(p); assert(p.rx == 0 && p.ry == 0);
    }
    // Neutral until the grab accepts a fresh edge, including a D-pad held before the window opens.
    kernel::W16(f + 0x920, 0);
    p = Chord(1); game::BlazingPad(0, p); Consumed(p); assert(p.ry == 0);
    kernel::W16(f + 0x920, 1);
    kernel::W32(f + 0x34, 0);
    p = Chord(1); game::BlazingPad(0, p); Consumed(p); assert(p.ry == 0);
    kernel::W32(f + 0x34, 7);
    p = Chord(1); game::BlazingPad(0, p); assert(p.ry == 32767);
    Fighter(0x1F, 2, 3); kernel::W32(opponent + 0x30, 0x20);
    p = Chord(8); game::BlazingPad(0, p); Consumed(p); assert(p.rx == 32767);
    kernel::W32(opponent, 0); Unchanged(); // stale/unrelated target
    kernel::W32(f, 0xFFFFFFFF); Unchanged(); // reject invalid pointers before reading them
    kernel::W32(f, f); Unchanged();
    kernel::W32(f, opponent); kernel::W32(opponent, f); kernel::W32(opponent + 0x30, 0x35); Unchanged();
}
