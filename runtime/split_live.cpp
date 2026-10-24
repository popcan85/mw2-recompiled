// MW2_SPLIT_LIVE: local clients beside the first on Xbox LIVE (split_live.h).
//
// The split-screen menu makes a player's local client active with the console
// command makeLocalClientActive (sub_821ACAD0), run by Cmd_ExecuteSingleCommand
// as his controller (sub_821AC1A0 is the menu's way of doing it). The command
// reads the controller from the command arguments, routes it to the local
// client of the same number (T_DATA_ControllerClient), marks that client
// active (T_DATA_ClientUIActive, 3216 bytes a client, the connection state at
// +2888 of the same record) and opens its menus. The routing is read only
// while cl_multi_gamepads_enabled is set (sub_821AB900); clear, as Xbox LIVE
// leaves it, the primary controller alone reaches a client.
//
// Online the title also makes one local client (T_CL_InitLocalClients), and
// the first frame with a second active one stops on "Too many active
// clients". So while the switch is on, the setup reads `splitscreen` from a
// copy of the dvar that says 2 -- set, but not the 1 the title stores, so the
// value the setup remembers never matches a real split-screen game's -- and
// makes four. The real dvar is not touched: the setup puts the remembered
// value back into the dvar it reads while a server runs, and the server's
// hunk, sized inside the setup, keeps reading the real one, as an online
// server must be sized for sv_maxclients. Each local client is then given the
// online room of eighteen players a snapshot instead of split screen's four,
// the most a private match has.
//
// A party keeps one member a player, and the title adds one a machine: its
// first player when it hosts, each machine that joins. Its own code knows a
// member may be local (the timeout spares a member whose XUID is signed in
// here, sub_823380C0), so a player who joins is added to the parties this
// machine hosts the way the host adds a machine that joined
// (PartyHost_AddPlayerAtSlot), in a free slot, with this machine's address and
// otherwise the first player's details. The lobby then lists him, the party
// state other machines are sent carries him, and the party's session --
// which the server checks a connecting player against (SV_DirectConnect,
// Session_FindRegisteredUser) -- registers him. He is removed from them as he
// leaves. Any other session the first player is registered in gets the
// others registered beside him (Session_RegisterUser).
//
// The machine's other players share its address, and the party finds a
// member by address (T_Party_FindMemberByAddr): for the member a message is
// from, whose details the host writes over his, and for the member of one
// party another is synced into. What this machine sends is its first
// player's, so the lookup passes over the others.
//
// A controller nobody plays at joins with START while this machine hosts a
// party in the menus, a press the title does not see: the sign-in screen
// opens first if nobody is signed in there, and choosing a profile joins once
// the title has read the profile in. Holding Back for two seconds, or signing
// out, leaves. Signing in, the title makes the new player's controller the
// one the menus answer to and every local client active, as for one player a
// machine; while players have joined, the menus stay the first player's and
// only the players in the game are active.
#include <ppc_recomp_shared.h>
#include "split_live.h"
#include "env.h"
#include "guest.h"
#include "log.h"
#include "signin.h"
#include "title.h"
#include "online/service.h"
#include "kernel/kernel.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    constexpr uint16_t kBack = 0x0020, kStart = 0x0010;
    constexpr auto kLeaveHold = std::chrono::seconds(2);

    // What the controllers asked for, from their reads to the next frame.
    std::mutex g_lock;
    uint16_t g_held[signin::kUsers]{};
    std::chrono::steady_clock::time_point g_backSince[signin::kUsers];
    bool g_backDone[signin::kUsers]{};
    bool g_join[signin::kUsers]{}, g_leave[signin::kUsers]{};
    bool g_hideStart[signin::kUsers]{};   // a START that joined, until it is let go
    std::atomic<bool> g_lobby{ false };          // as of the last frame
    std::atomic<bool> g_joined[signin::kUsers]{};
}

#if defined(T_Cmd_ExecuteSingleCommand) && defined(T_PartyHost_AddPlayerAtSlot)
namespace
{
    constexpr uint32_t kClients = 4;
    constexpr uint32_t kParties[] = { T_DATA_Party0, T_DATA_Party1 };
    constexpr uint8_t kInParty = 3;   // a member's status from here up

    bool g_asked[signin::kUsers]{};   // the sign-in screen was opened for a join
    // A player signed in for a join waits for the title to read his profile in.
    std::chrono::steady_clock::time_point g_signedInAt[signin::kUsers];
    bool g_waiting[signin::kUsers]{};
    constexpr auto kSettle = std::chrono::milliseconds(1500);
    // The first player's controller -- the first, signed in to Live from the
    // start -- which the menus answer to.
    constexpr int32_t kFirst = 0;

    struct State
    {
        int32_t multi = 0, primary = 0, count = 0;
        std::array<int32_t, kClients> client{};
        std::array<uint8_t, kClients> active{};
        std::array<int32_t, kClients> connection{}, room{};
        bool operator==(const State&) const = default;
    };

    State Read()
    {
        State s;
        s.multi = *GuestPtr<be32>(T_DATA_MultiGamepads);
        s.primary = *GuestPtr<be32>(T_DATA_PrimaryController);
        s.count = *GuestPtr<be32>(T_DATA_LocalClientCount);
        const uint32_t clients = *GuestPtr<be32>(T_DATA_LocalClients);
        for (uint32_t i = 0; i < kClients; i++)
        {
            s.client[i] = *GuestPtr<be32>(T_DATA_ControllerClient + 4 * i);
            s.active[i] = *GuestPtr<uint8_t>(T_DATA_ClientUIActive + T_CONN_STATE_STRIDE * i);
            s.connection[i] = *GuestPtr<be32>(T_DATA_ClientConnState + T_CONN_STATE_STRIDE * i);
            s.room[i] = clients && int32_t(i) < s.count
                            ? int32_t(*GuestPtr<be32>(clients + i * T_LOCAL_CLIENT_STRIDE + T_LOCAL_CLIENT_PARSE_ROOM))
                            : 0;
        }
        return s;
    }

    void Note(const State& s)
    {
        std::string text;
        char part[112];
        for (uint32_t i = 0; i < kClients; i++)
        {
            std::snprintf(part, sizeof part, " | controller %u -> client %d, client %u %s, connection %d, room %d", i,
                          s.client[i], i, s.active[i] ? "active" : "inactive", s.connection[i], s.room[i]);
            text += part;
        }
        LOGI("splitlive: %d local clients, multi gamepads %d, primary controller %d%s", s.count, s.multi, s.primary,
             text.c_str());
    }

    uint32_t Member(uint32_t party, uint32_t index) { return party + T_PARTY_MEMBERS + index * T_PARTY_MEMBER_SIZE; }

    int32_t MemberOf(uint32_t party, uint64_t xuid)
    {
        for (uint32_t i = 0; i < T_SESSION_SLOTS; i++)
            if (*GuestPtr<uint8_t>(Member(party, i)) >= kInParty &&
                uint64_t(*GuestPtr<be64>(Member(party, i) + T_MEMBER_XUID)) == xuid)
                return int32_t(i);
        return -1;
    }

    bool Hosting(uint32_t party) { return *GuestPtr<be32>(party + T_PARTY_HOSTING) != 0; }

    // The parties, as the log tells them whenever they change: who hosts, and
    // the members, the first player's marked.
    void NoteParties()
    {
        static std::string last;
        std::string text;
        char part[96];
        for (uint32_t party : kParties)
        {
            const uint32_t me = *GuestPtr<be32>(party + T_PARTY_ME);
            std::snprintf(part, sizeof part, " | party %08x%s:", party, Hosting(party) ? " (hosting)" : "");
            text += part;
            for (uint32_t i = 0; i < T_SESSION_SLOTS; i++)
            {
                const uint32_t m = Member(party, i);
                const uint8_t status = *GuestPtr<uint8_t>(m);
                if (!status) continue;
                char name[33]{};
                std::memcpy(name, GuestPtr<char>(m + T_MEMBER_NAME), 32);
                std::snprintf(part, sizeof part, " %u%s \"%s\" (status %u)", i, i == me ? "*" : "", name, status);
                text += part;
            }
        }
        if (text == last) return;
        last = text;
        LOGI("splitlive: parties%s", text.c_str());
    }

    // The command, run as `controller` the way the menus run it.
    void Run(PPCContext& ctx, uint8_t* base, uint32_t controller, const char* command)
    {
        const uint32_t sp = ctx.r1.u32;
        if (sp < 0x1000) return;
        const uint32_t scratch = (sp - 0x100) & ~0xFu;
        std::memcpy(base + scratch, command, std::strlen(command) + 1);
        const PPCContext saved = ctx;
        ctx.r1.u64 = (sp - 0x200) & ~0xFull;
        ctx.r3.u64 = controller;   // its local client, the same number once routed
        ctx.r4.u64 = controller;
        ctx.r5.u64 = scratch;
        GUEST_FUNC(T_Cmd_ExecuteSingleCommand)(ctx, base);
        ctx = saved;
        LOGI("splitlive: ran \"%s\" as controller %u", command, controller);
    }

    // The player at `user` into `party`, if this machine hosts it, as the host
    // adds a machine that joins: PartyHost_AddPlayerAtSlot(party, slot,
    // address (8 bytes, then 4 in the high word), &xuid, &8 bytes, name, clan
    // tag; on the stack the member's details), which fills the member, gives
    // him the address and registers him in the party's session. All but the
    // name and the XUID are the first player's: the same machine.
    void AddToParty(PPCContext& ctx, uint8_t* base, uint32_t party, uint32_t user)
    {
        const uint64_t xuid = kernel::LiveXuid(user);
        const uint32_t session = *GuestPtr<be32>(party);
        const uint32_t me = *GuestPtr<be32>(party + T_PARTY_ME);
        if (!xuid || !Hosting(party) || !session || me >= T_SESSION_SLOTS || MemberOf(party, xuid) >= 0) return;
        const uint32_t mine = Member(party, me);
        if (*GuestPtr<uint8_t>(mine) < kInParty) return;

        uint32_t slots = uint32_t(*GuestPtr<be32>(session + T_SESSION_PRIVATE)) + uint32_t(*GuestPtr<be32>(session + T_SESSION_PUBLIC));
        if (slots > T_SESSION_SLOTS) slots = T_SESSION_SLOTS;
        int32_t slot = -1;
        for (uint32_t i = 0; i < slots && slot < 0; i++)
            if (!*GuestPtr<uint8_t>(Member(party, i)) && !*GuestPtr<uint8_t>(session + T_SESSION_USERS + i * T_SESSION_USER_SIZE))
                slot = int32_t(i);
        if (slot < 0)
        {
            LOGW("splitlive: party %08x is full; controller %u is not in it", party, user);
            return;
        }

        const PPCContext saved = ctx;
        const uint32_t sp = (ctx.r1.u32 - 0x800) & ~0xFu;
        const uint32_t data = sp + 0x100;
        std::memset(GuestPtr<uint8_t>(data), 0, 0x40);
        *GuestPtr<be64>(data) = xuid;
        std::memcpy(GuestPtr<uint8_t>(data + 8), GuestPtr<uint8_t>(mine + T_MEMBER_MACHINE), 8);
        std::snprintf(GuestPtr<char>(data + 16), 32, "%s", signin::At(user).name.c_str());
        std::memcpy(GuestPtr<uint8_t>(data + 48), GuestPtr<uint8_t>(mine + T_MEMBER_CLANTAG), 5);
        // The details, where the callee reads them past its 160-byte frame.
        *GuestPtr<uint8_t>(sp + 87) = *GuestPtr<uint8_t>(mine + 1);
        const uint32_t words[][2] = { { 92, 124 }, { 100, 128 }, { 108, 148 }, { 116, 152 },
                                      { 124, 156 }, { 132, 100 }, { 148, 168 } };
        for (const auto& w : words) *GuestPtr<be32>(sp + w[0]) = uint32_t(*GuestPtr<be32>(mine + w[1]));
        *GuestPtr<uint8_t>(sp + 143) = *GuestPtr<uint8_t>(mine + 164);
        const uint32_t address = session + T_SESSION_USERS + me * T_SESSION_USER_SIZE + T_SESSION_USER_ADDRESS;

        ctx.r1.u64 = sp;
        ctx.r3.u64 = party;
        ctx.r4.u64 = uint32_t(slot);
        ctx.r5.u64 = uint64_t(*GuestPtr<be64>(address));
        ctx.r6.u64 = uint64_t(uint32_t(*GuestPtr<be32>(address + 8))) << 32;
        ctx.r7.u64 = data;
        ctx.r8.u64 = data + 8;
        ctx.r9.u64 = data + 16;
        ctx.r10.u64 = data + 48;
        GUEST_FUNC(T_PartyHost_AddPlayerAtSlot)(ctx, base);
        const bool added = ctx.r3.u8 != 0;
        ctx = saved;
        LOGI("splitlive: controller %u (\"%s\", xuid %016llx) %s party %08x at slot %d", user,
             signin::At(user).name.c_str(), (unsigned long long)xuid, added ? "added to" : "NOT added to", party, slot);
    }

    void RemoveFromParty(PPCContext& ctx, uint8_t* base, uint32_t party, uint32_t user)
    {
        const uint64_t xuid = kernel::LiveXuid(user);
        const int32_t slot = xuid && Hosting(party) ? MemberOf(party, xuid) : -1;
        if (slot < 0 || uint32_t(slot) == uint32_t(*GuestPtr<be32>(party + T_PARTY_ME))) return;
        const PPCContext saved = ctx;
        ctx.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFull;
        ctx.r3.u64 = party;
        ctx.r4.u64 = uint32_t(slot);
        ctx.r5.u64 = 0;                      // nobody to tell: he is here
        ctx.r6.u64 = T_DATA_EmptyString;
        GUEST_FUNC(T_PartyHost_RemoveMember)(ctx, base);
        ctx = saved;
        LOGI("splitlive: controller %u left party %08x (slot %d)", user, party, slot);
    }

    // In the Xbox LIVE menus, hosting a party, with no game joined.
    bool InLobby(const State& s)
    {
        if (s.connection[0] != 0) return false;
        for (uint32_t party : kParties)
            if (Hosting(party)) return true;
        return false;
    }

    void Join(PPCContext& ctx, uint8_t* base, uint32_t user)
    {
        g_joined[user] = true;
        if (!*GuestPtr<be32>(T_DATA_MultiGamepads))
        {
            *GuestPtr<be32>(T_DATA_MultiGamepads) = 1;
            LOGI("splitlive: every controller routed to its own client (cl_multi_gamepads_enabled)");
        }
        Run(ctx, base, user, "makeLocalClientActive");
        for (uint32_t party : kParties) AddToParty(ctx, base, party, user);
    }

    void Leave(PPCContext& ctx, uint8_t* base, uint32_t user)
    {
        g_joined[user] = false;
        for (uint32_t party : kParties) RemoveFromParty(ctx, base, party, user);
        Run(ctx, base, user, "makeLocalClientInactive");
    }

    // In the lobby with players joined: the menus stay the first player's, and
    // only the players in the game are active.
    void Keep(const State& s)
    {
        bool any = false;
        for (uint32_t user = 1; user < signin::kUsers; user++) any = any || g_joined[user];
        if (!any) return;
        if (s.primary != kFirst || int32_t(*GuestPtr<be32>(T_DATA_MenuController)) != kFirst)
        {
            *GuestPtr<be32>(T_DATA_PrimaryController) = uint32_t(kFirst);
            *GuestPtr<be32>(T_DATA_MenuController) = uint32_t(kFirst);
            LOGI("splitlive: the menus stay with the first controller");
        }
        if (!s.multi) *GuestPtr<be32>(T_DATA_MultiGamepads) = 1;
        for (uint32_t client = 0; client < kClients; client++)
        {
            const uint8_t want = client == uint32_t(kFirst) || (client < signin::kUsers && g_joined[client]);
            if (s.active[client] != want)
                *GuestPtr<uint8_t>(T_DATA_ClientUIActive + T_CONN_STATE_STRIDE * client) = want;
        }
    }
}

#ifdef T_Party_FindMemberByAddr
namespace
{
    // One of this machine's players beside the first.
    bool Beside(uint32_t party, int32_t index)
    {
        if (index < 0 || index >= int32_t(T_SESSION_SLOTS)) return false;
        const uint64_t xuid = *GuestPtr<be64>(Member(party, uint32_t(index)) + T_MEMBER_XUID);
        for (uint32_t user = 1; user < signin::kUsers; user++)
            if (xuid && xuid == kernel::LiveXuid(user)) return true;
        return false;
    }
}

GUEST_HOOK(T_Party_FindMemberByAddr)
{
    const PPCContext call = ctx;
    GUEST_ORIG(T_Party_FindMemberByAddr)(ctx, base);
    if (!splitlive::On()) return;
    // Past this machine's other players, to the next member at the address.
    while (Beside(call.r3.u32, ctx.r3.s32))
    {
        const int32_t past = ctx.r3.s32;
        const PPCContext result = ctx;
        ctx = call;
        ctx.r6.u64 = uint32_t(past + 1);
        if (past + 1 >= int32_t(T_SESSION_SLOTS))
        {
            ctx = result;
            ctx.r3.s64 = -1;
            return;
        }
        GUEST_ORIG(T_Party_FindMemberByAddr)(ctx, base);
        if (ctx.r3.s32 >= 0 && ctx.r3.s32 <= past)   // the fallback walk found him again
        {
            ctx.r3.s64 = -1;
            return;
        }
    }
}
#endif

#ifdef T_PartyHost_StartParty
// A party started (or started again) while players beside the first are in
// the game: they are in it too.
GUEST_HOOK(T_PartyHost_StartParty)
{
    const uint32_t party = ctx.r3.u32;
    GUEST_ORIG(T_PartyHost_StartParty)(ctx, base);
    if (!splitlive::On()) return;
    for (uint32_t user = 1; user < signin::kUsers; user++)
        if (g_joined[user]) AddToParty(ctx, base, party, user);
}
#endif
#endif

#ifdef T_CL_InitLocalClients
namespace
{
    uint32_t g_realSplitscreen = 0;   // the dvar, while the setup reads the copy

    uint8_t Bool(uint32_t pointerAt)
    {
        const uint32_t dvar = *GuestPtr<be32>(pointerAt);
        return dvar ? *GuestPtr<uint8_t>(dvar + 12) : 0;
    }

    // Each local client's room for players a snapshot, from split screen's four
    // to the online eighteen, allocated as the setup allocates.
    void Widen(PPCContext& ctx, uint8_t* base)
    {
        constexpr uint32_t kOnlinePlayers = 18;
        const uint32_t count = *GuestPtr<be32>(T_DATA_LocalClientCount);
        const uint32_t clients = *GuestPtr<be32>(T_DATA_LocalClients);
        for (uint32_t i = 0; i < count && clients; i++)
        {
            const uint32_t client = clients + i * T_LOCAL_CLIENT_STRIDE;
            auto& room = *GuestPtr<be32>(client + T_LOCAL_CLIENT_PARSE_ROOM);
            const uint32_t want = uint32_t(*GuestPtr<be32>(client + T_LOCAL_CLIENT_SNAPSHOTS)) * kOnlinePlayers;
            if (room >= want) continue;
            const PPCContext saved = ctx;
            ctx.r3.u64 = want * T_PARSE_CLIENT_SIZE;
            ctx.r4.u64 = 4;
            ctx.r5.u64 = 4;
            GUEST_FUNC(T_Hunk_Alloc)(ctx, base);
            const uint32_t array = ctx.r3.u32;
            if (array)
            {
                ctx.r4.u64 = 0;
                ctx.r5.u64 = want * T_PARSE_CLIENT_SIZE;
                GUEST_FUNC(T_memset)(ctx, base);
                *GuestPtr<be32>(client + T_LOCAL_CLIENT_PARSE) = array;
                room = want;
            }
            ctx = saved;
        }
    }
}

GUEST_HOOK(T_CL_InitLocalClients)
{
    const uint32_t real = splitlive::On() ? uint32_t(*GuestPtr<be32>(T_DATA_SplitscreenDvar)) : 0;
    if (!real || *GuestPtr<uint8_t>(real + 12))
    {
        GUEST_ORIG(T_CL_InitLocalClients)(ctx, base);
        return;
    }
    static uint32_t copy = 0;
    constexpr uint32_t kDvarBytes = 256;
    if (!copy) copy = kernel::AllocateGuest(kDvarBytes);
    if (!copy)
    {
        GUEST_ORIG(T_CL_InitLocalClients)(ctx, base);
        return;
    }
    std::memcpy(GuestPtr<uint8_t>(copy), GuestPtr<uint8_t>(real), kDvarBytes);
    *GuestPtr<uint8_t>(copy + 12) = 2;

    const uint32_t before = *GuestPtr<be32>(T_DATA_LocalClientCount);
    const uint8_t running = Bool(T_DATA_SvRunningDvar);
    *GuestPtr<be32>(T_DATA_SplitscreenDvar) = copy;
    g_realSplitscreen = real;
    GUEST_ORIG(T_CL_InitLocalClients)(ctx, base);
    g_realSplitscreen = 0;
    *GuestPtr<be32>(T_DATA_SplitscreenDvar) = real;
    Widen(ctx, base);
    LOGI("splitlive: local clients set up for Xbox LIVE (server running %u): %u before, %u now",
         running, before, uint32_t(*GuestPtr<be32>(T_DATA_LocalClientCount)));
}

GUEST_HOOK(T_SV_InitHunk)
{
    const uint32_t copy = g_realSplitscreen ? uint32_t(*GuestPtr<be32>(T_DATA_SplitscreenDvar)) : 0;
    if (copy) *GuestPtr<be32>(T_DATA_SplitscreenDvar) = g_realSplitscreen;
    GUEST_ORIG(T_SV_InitHunk)(ctx, base);
    if (copy) *GuestPtr<be32>(T_DATA_SplitscreenDvar) = copy;
}
#endif

#ifdef T_Session_RegisterUser
PPC_FUNC_IMPL(GUEST_ORIG(T_Session_RegisterUser));

namespace
{
    bool g_registering = false;

    bool Holds(uint32_t session, uint64_t xuid)
    {
        for (uint32_t i = 0; i < T_SESSION_SLOTS; i++)
        {
            const uint32_t at = session + T_SESSION_USERS + i * T_SESSION_USER_SIZE;
            if (*GuestPtr<uint8_t>(at) && uint64_t(*GuestPtr<be64>(at + T_SESSION_USER_XUID)) == xuid) return true;
        }
        return false;
    }

    bool PartySession(uint32_t session)
    {
#ifdef T_PartyHost_AddPlayerAtSlot
        for (uint32_t party : kParties)
            if (uint32_t(*GuestPtr<be32>(party)) == session) return true;
#endif
        return false;
    }
}

// The first player put in a session that is not a party's (which the party
// keeps its members in itself): the other active local players beside him,
// in free slots and otherwise as he was. Session_RegisterUser(controller,
// session, xuid, private, slot, natType, ?, address; on the stack one more).
GUEST_HOOK(T_Session_RegisterUser)
{
    const uint32_t session = ctx.r4.u32;
    const bool first = splitlive::On() && !g_registering && ctx.r5.u64 == kernel::LiveXuid(0) && !PartySession(session);
    const PPCContext call = ctx;
    const uint64_t stacked = *GuestPtr<be64>(ctx.r1.u32 + 88);
    GUEST_ORIG(T_Session_RegisterUser)(ctx, base);
    if (!first) return;
    for (uint32_t user = 1; user < signin::kUsers; user++)
    {
        const uint64_t xuid = kernel::LiveXuid(user);
        if (!xuid || !g_joined[user] || Holds(session, xuid)) continue;
        int32_t slot = -1;
        for (uint32_t i = 0; i < T_SESSION_SLOTS && slot < 0; i++)
            if (!*GuestPtr<uint8_t>(session + T_SESSION_USERS + i * T_SESSION_USER_SIZE)) slot = int32_t(i);
        if (slot < 0) continue;
        const PPCContext saved = ctx;
        ctx = call;
        const uint32_t sp = (call.r1.u32 - 0x400) & ~0xFu;
        *GuestPtr<be64>(sp + 88) = stacked;   // the ninth argument, where the callee reads it
        ctx.r1.u64 = sp;
        ctx.r5.u64 = xuid;
        ctx.r7.u64 = uint32_t(slot);
        g_registering = true;
        GUEST_ORIG(T_Session_RegisterUser)(ctx, base);
        g_registering = false;
        ctx = saved;
        LOGI("splitlive: controller %u (xuid %016llx) registered in session %08x at slot %d, beside the first player",
             user, (unsigned long long)xuid, session, slot);
    }
}
#endif

bool splitlive::On()
{
    static const bool on = env::Flag("MW2_SPLIT_LIVE") && online::Live();
    return on;
}

uint16_t splitlive::Input(uint32_t user, uint16_t buttons)
{
    if (!On() || user == 0 || user >= signin::kUsers) return 0;
    std::lock_guard g(g_lock);
    const uint16_t pressed = buttons & ~g_held[user];
    g_held[user] = buttons;
    // START at a controller not in the game is a join, not the menu's button,
    // and stays hidden until it is let go, the player in the game by then.
    if ((pressed & kStart) && g_lobby && !g_joined[user]) g_hideStart[user] = true;
    if (!(buttons & kStart)) g_hideStart[user] = false;
    const uint16_t hidden = g_hideStart[user] ? kStart : 0;
    if ((pressed & kStart) && !(buttons & kBack)) g_join[user] = true;
    const bool back = (buttons & (kBack | kStart)) == kBack;
    const auto now = std::chrono::steady_clock::now();
    if (!back) g_backDone[user] = false;
    else if (pressed & kBack) g_backSince[user] = now;
    else if (!g_backDone[user] && now - g_backSince[user] >= kLeaveHold)
    {
        g_backDone[user] = true;
        g_leave[user] = true;
    }
    return hidden;
}

void splitlive::Frame(PPCContext& ctx, uint8_t* base)
{
    if (!On()) return;
#if defined(T_Cmd_ExecuteSingleCommand) && defined(T_PartyHost_AddPlayerAtSlot)
    static State last;
    static bool first = true;
    const State now = Read();
    if (first || !(now == last)) Note(now);
    first = false;
    last = now;
    NoteParties();

    const bool lobby = InLobby(now);
    g_lobby = lobby;
    // Out of Xbox LIVE (no party hosted, no game), nobody is in the game.
    if (!lobby && now.connection[0] == 0)
        for (uint32_t user = 1; user < signin::kUsers; user++) g_joined[user] = false;
    for (uint32_t user = 1; user < signin::kUsers; user++)
    {
        bool join, leave;
        {
            std::lock_guard g(g_lock);
            join = g_join[user];
            leave = g_leave[user];
            g_join[user] = g_leave[user] = false;
        }
        const bool joined = g_joined[user];
        const bool signedIn = signin::At(user).signedIn;
        const auto time = std::chrono::steady_clock::now();

        // Joining: START, then the sign-in screen when nobody is signed in,
        // then the title reads the profile in.
        if (join && !joined)
        {
            if (!lobby)
                LOGI("splitlive: START on controller %u: players join in the Xbox LIVE lobby this machine hosts", user);
            else if (signedIn)
                Join(ctx, base, user);
            else if (!signin::Open())
            {
                signin::AskFor(user);
                g_asked[user] = true;
            }
        }
        if (g_asked[user] && !signin::Open())
        {
            g_asked[user] = false;
            if (signin::At(user).signedIn)
            {
                g_waiting[user] = true;
                g_signedInAt[user] = time;
            }
        }
        if (g_waiting[user] && time - g_signedInAt[user] >= kSettle)
        {
            g_waiting[user] = false;
            if (signin::At(user).signedIn && lobby && !g_joined[user]) Join(ctx, base, user);
        }

        // Leaving: Back held, or signing out.
        if (joined && lobby && (leave || !signedIn)) Leave(ctx, base, user);
    }
    if (lobby) Keep(Read());
#else
    (void)ctx; (void)base;
#endif
}
