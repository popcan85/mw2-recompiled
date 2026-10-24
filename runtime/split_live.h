#pragma once
// MW2_SPLIT_LIVE: more than one player of this machine on Xbox LIVE.
//
// The multiplayer plays split screen offline only. Choosing Xbox LIVE sets
// `splitscreen 0`, and with cl_multi_gamepads_enabled clear the title reads the
// primary controller alone, so a second player signed in to Live opens a party
// of his own instead of joining the first's. The engine underneath is the
// split screen one -- four local clients, each with its own connection, which
// CL_ConnectFromParty connects one by one -- so what is missing is the step
// the split-screen menu takes for each player who joins: makeLocalClientActive,
// run as his controller, with the controllers routed to their clients.
//
// While the switch is on, holding Back for a second on the second to fourth
// controller, with a player signed in to Live there, makes that controller's
// local client active (or inactive again). The log says whenever the routing,
// the active clients or their connection states change.
#include <cstdint>

struct PPCContext;

namespace splitlive
{
    // MW2_SPLIT_LIVE, with an online service that carries lobbies.
    bool On();

    // Every read of a controller: its buttons (XINPUT_GAMEPAD).
    void Input(uint32_t user, uint16_t buttons);

    // Once a frame on the title's main thread.
    void Frame(PPCContext& ctx, uint8_t* base);
}
