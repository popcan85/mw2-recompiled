#pragma once
// MW2_BENCHMARK=<players>: the multiplayer measures itself and ends
// (benchmark.cpp). From the main menu it does what the Split Screen menu
// does for that many players -- the split-screen settings, a profile signed in
// at each controller beside the first (for this run only), each controller's
// client made active -- and starts a match on MW2_BENCHMARK_MAP (mp_terminal).
// Its controllers are driven here, whether a pad is there or not: A now and
// then until play starts, then every view turning slowly. Once play has gone
// on for a moment, it counts the title's frames for MW2_BENCHMARK_SECONDS (15),
// writes what it measured to MW2_BENCHMARK_OUT (benchmark.txt) and ends the
// program. The launcher's OPTIMISE runs it at one setting after another.
#include <cstdint>

struct PPCContext;

namespace benchmark
{
    // MW2_BENCHMARK, 1 to 4 players.
    bool On();
    uint32_t Players();

    // Once a frame on the title's main thread.
    void Frame(PPCContext& ctx, uint8_t* base);

    // Whether the run drives this controller, a pad there or not.
    bool Drives(uint32_t user);
    // What the run presses on a controller it drives, over what was read.
    void Drive(uint32_t user, uint16_t& buttons, int16_t axes[4]);

    // A menu the title drew this frame, by name.
    void MenuPainted(const char* name);
}
