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
// While the switch is on, START at the second to fourth controller in the
// Xbox LIVE lobby this machine hosts joins the player there (the sign-in
// screen opens first if nobody is signed in), and holding Back for two
// seconds leaves. The log says whenever the routing, the active clients, their
// connection states or the parties change.
#include <cstdint>

struct PPCContext;

namespace splitlive
{
    // MW2_SPLIT_LIVE, with an online service that carries lobbies.
    bool On();

    // Every read of a controller: its buttons (XINPUT_GAMEPAD). Returns the
    // buttons the title is not to see.
    uint16_t Input(uint32_t user, uint16_t buttons);

    // Once a frame on the title's main thread.
    void Frame(PPCContext& ctx, uint8_t* base);

    // A connectionless packet offered to `localClient` (its command tokenized)
    // that is another local client's: a server's challengeResponse names the
    // key the challenge was asked with, which several players of one machine
    // ask for each their own.
    bool ForAnotherClient(PPCContext& ctx, uint8_t* base, uint32_t localClient);

    // Each local client beside the first is a port of its own to the network,
    // its socket's plus kPortStep for each: a server tells the players of one
    // machine apart by address. SendingClient is the local client the title
    // sends for now (0: the first, or the machine itself); ReceivedFor says
    // which one a datagram arrived for, by its port, and PacketForAnother
    // whether a packet from the network (address type 4) offered to
    // `localClient` is another's.
    constexpr uint16_t kPortStep = 100;
    uint32_t SendingClient();
    void ReceivedFor(uint32_t localClient);
    bool PacketForAnother(uint32_t localClient, uint32_t addressType);

    // The program is ending: the hosts of the parties this machine is in are
    // told its players leave, as the title tells them when it leaves a party,
    // so that their lobbies drop them at once instead of when they time out.
    // Waits for the title's next frame, a moment at most.
    void Goodbye();
}
