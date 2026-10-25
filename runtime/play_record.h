#pragma once
// The frame rate of real play, kept for the launcher (play_record.cpp).
//
// Once a match has gone on for a moment, the multiplayer counts its frames
// until play ends -- the match over, left, or the program closed -- and, for
// a match of 20 seconds or more, adds a line to performance.txt beside the
// executables: when, the map, whether this machine hosted, its players in
// the match and, hosting, everyone's, the settings it drew with, the average
// rate and the slowest hundredth of the frames as a rate. The launcher's
// OPTIMISE sets what a benchmark found beside it: a benchmark has nobody from
// other machines, and real play can ask for more than it does.
#include <cstdint>

struct PPCContext;

namespace playrecord
{
    // Once a frame on the title's main thread.
    void Frame(PPCContext& ctx, uint8_t* base);
    // The program is ending: a match in play is written as it stands.
    void End();
}
