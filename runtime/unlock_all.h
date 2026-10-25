#pragma once
// MW2_UNLOCK_ALL: every player at the end of rank 70 with everything unlocked
// (unlock_all.cpp), whoever signs in at whichever controller.
#include <cstdint>

struct PPCContext;

namespace unlockall
{
    // Once a frame on the title's main thread.
    void Frame(PPCContext& ctx, uint8_t* base);
}
