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
// here), so a player who joins is added to the parties this machine hosts the
// way the host puts himself in his own (PartyHost_AddLocalPlayer, from his
// controller's profile), in a free slot. The lobby then lists him, the party
// state other machines are sent carries him, and the party's session --
// which the server checks a connecting player against (SV_DirectConnect,
// Session_FindRegisteredUser) -- registers him. He is removed from them as he
// leaves. Any other session the first player is registered in gets the
// others registered beside him (Session_RegisterUser). The title gives every
// member signed in here the name of the controller it refreshes them for
// (Party_RefreshLocalMembers), so each is given his own back after it.
//
// In a party another machine hosts, a player who joins is sent to the host as
// the first player was, with the title's own memberJoin from his controller,
// once the party lists the first player. The host's memberJoin handler adds a
// XUID the member at the sender's address does not have, so he is added with
// that machine's address, and the host keeps him as one of that machine's
// players beside its first: heard from whenever it is, and put in each party
// it hosts that has its first player.
//
// A machine's players share its address, and the party finds a member by
// address (T_Party_FindMemberByAddr): for the member a message is from, whose
// details the host writes over his, and for the member of one party another
// is synced into. What a machine sends is its first player's, so the lookup
// passes over the others, unless nobody else is there.
//
// A controller nobody plays at joins with START while this machine is in a
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
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
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
    std::chrono::steady_clock::time_point g_readAt[signin::kUsers];   // a pad there was read
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
    bool Active(uint32_t party) { return *GuestPtr<be32>(party + T_PARTY_ACTIVE) != 0; }
    uint64_t XuidAt(uint32_t party, uint32_t index) { return *GuestPtr<be64>(Member(party, index) + T_MEMBER_XUID); }

    // A member's address, in his session slot (the member's own number).
    uint32_t AddressOf(uint32_t party, uint32_t index)
    {
        const uint32_t session = *GuestPtr<be32>(party);
        return session ? session + T_SESSION_USERS + index * T_SESSION_USER_SIZE + T_SESSION_USER_ADDRESS : 0;
    }

    // The players of other machines beside their first, as this machine hosts
    // them, each with his machine's first player's XUID: the address a player
    // beside the first is added with is what his memberJoin came from, which
    // need not be the one the first is registered at.
    std::map<uint64_t, uint64_t> g_beside;

    // One of this machine's players beside the first, or another machine's.
    bool Beside(uint32_t party, int32_t index)
    {
        if (index < 0 || index >= int32_t(T_SESSION_SLOTS)) return false;
        const uint64_t xuid = XuidAt(party, uint32_t(index));
        if (!xuid) return false;
        for (uint32_t user = 1; user < signin::kUsers; user++)
            if (xuid == kernel::LiveXuid(user)) return true;
        return g_beside.count(xuid) != 0;
    }

    // The first player of the machine one of another machine's players beside
    // its first plays on, or -1.
    int32_t FirstAt(uint32_t party, uint32_t index)
    {
        const auto first = g_beside.find(XuidAt(party, index));
        return first == g_beside.end() ? -1 : MemberOf(party, first->second);
    }

    // A slot nobody has in the party or its session, or -1.
    int32_t FreeSlot(uint32_t party)
    {
        const uint32_t session = *GuestPtr<be32>(party);
        uint32_t slots = uint32_t(*GuestPtr<be32>(session + T_SESSION_PRIVATE)) + uint32_t(*GuestPtr<be32>(session + T_SESSION_PUBLIC));
        if (slots > T_SESSION_SLOTS) slots = T_SESSION_SLOTS;
        for (uint32_t i = 0; i < slots; i++)
            if (!*GuestPtr<uint8_t>(Member(party, i)) && !*GuestPtr<uint8_t>(session + T_SESSION_USERS + i * T_SESSION_USER_SIZE))
                return int32_t(i);
        return -1;
    }

    // The parties, as the log tells them whenever they change: who hosts, and
    // the members, the first player's marked.
    void NoteParties()
    {
        static std::string last;
        std::string text;
        char part[96];
        for (uint32_t party : kParties)
        {
            const uint32_t me = *GuestPtr<be32>(party + T_PARTY_HOST);
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

    // The player at `user` into `party`, if this machine hosts it and its
    // first player is in it, as the host puts himself in his own party:
    // PartyHost_AddLocalPlayer(party, controller, slot) fills the member from
    // the controller's profile and registers him in the party's session. It
    // also stores the new member's byte at +1 as the party's own, which stays
    // the first player's.
    bool AddToParty(PPCContext& ctx, uint8_t* base, uint32_t party, uint32_t user)
    {
        const uint64_t xuid = kernel::LiveXuid(user);
        const uint32_t me = *GuestPtr<be32>(party + T_PARTY_HOST);
        if (!xuid || !Hosting(party) || !*GuestPtr<be32>(party) || me >= T_SESSION_SLOTS || MemberOf(party, xuid) >= 0) return false;
        if (*GuestPtr<uint8_t>(Member(party, me)) < kInParty) return false;
        const int32_t slot = FreeSlot(party);
        if (slot < 0)
        {
            LOGW("splitlive: party %08x is full; controller %u is not in it", party, user);
            return false;
        }
        const uint32_t detail = *GuestPtr<be32>(party + T_PARTY_LOCAL_DETAIL);
        const PPCContext saved = ctx;
        ctx.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFull;
        ctx.r3.u64 = party;
        ctx.r4.u64 = user;
        ctx.r5.u64 = uint32_t(slot);
        GUEST_FUNC(T_PartyHost_AddLocalPlayer)(ctx, base);
        ctx = saved;
        *GuestPtr<be32>(party + T_PARTY_LOCAL_DETAIL) = detail;
        char name[33]{};
        std::memcpy(name, GuestPtr<char>(Member(party, uint32_t(slot)) + T_MEMBER_NAME), 32);
        const bool added = MemberOf(party, xuid) >= 0;
        LOGI("splitlive: controller %u (\"%s\", xuid %016llx) %s party %08x at slot %d", user, name,
             (unsigned long long)xuid, added ? "added to" : "NOT added to", party, slot);
        return added;
    }

    // Another machine's player beside its first, member `index` of the party
    // `from`, into `party`, as the host adds a machine that joins:
    // PartyHost_AddPlayerAtSlot(party, slot, address (8 bytes, then 4 in the
    // high word), &xuid, &8 bytes, name, clan tag; on the stack the member's
    // details), which fills the member, gives him the address and registers him
    // in the party's session. Everything is what `from` has for him.
    bool CopyToParty(PPCContext& ctx, uint8_t* base, uint32_t party, uint32_t from, uint32_t index)
    {
        const uint32_t source = Member(from, index);
        const uint64_t xuid = XuidAt(from, index);
        const uint32_t address = AddressOf(from, index);
        if (!xuid || !address || !Hosting(party) || !*GuestPtr<be32>(party) || MemberOf(party, xuid) >= 0) return false;
        const int32_t slot = FreeSlot(party);
        char name[33]{};
        std::memcpy(name, GuestPtr<char>(source + T_MEMBER_NAME), 32);
        if (slot < 0)
        {
            LOGW("splitlive: party %08x is full; \"%s\" is not in it", party, name);
            return false;
        }

        const PPCContext saved = ctx;
        const uint32_t sp = (ctx.r1.u32 - 0x800) & ~0xFu;
        const uint32_t data = sp + 0x100;
        std::memset(GuestPtr<uint8_t>(data), 0, 0x40);
        *GuestPtr<be64>(data) = xuid;
        std::memcpy(GuestPtr<uint8_t>(data + 8), GuestPtr<uint8_t>(source + T_MEMBER_MACHINE), 8);
        std::memcpy(GuestPtr<char>(data + 16), name, 32);
        std::memcpy(GuestPtr<uint8_t>(data + 48), GuestPtr<uint8_t>(source + T_MEMBER_CLANTAG), 5);
        // The details, where the callee reads them past its 160-byte frame.
        *GuestPtr<uint8_t>(sp + 87) = *GuestPtr<uint8_t>(source + 1);
        const uint32_t words[][2] = { { 92, 124 }, { 100, 128 }, { 108, 148 }, { 116, 152 },
                                      { 124, 156 }, { 132, 100 }, { 148, 168 } };
        for (const auto& w : words) *GuestPtr<be32>(sp + w[0]) = uint32_t(*GuestPtr<be32>(source + w[1]));
        *GuestPtr<uint8_t>(sp + 143) = *GuestPtr<uint8_t>(source + 164);

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
        LOGI("splitlive: \"%s\" (xuid %016llx, beside his machine's first player) %s party %08x at slot %d", name,
             (unsigned long long)xuid, added ? "added to" : "NOT added to", party, slot);
        return added;
    }

    void RemoveFromParty(PPCContext& ctx, uint8_t* base, uint32_t party, uint32_t user)
    {
        const uint64_t xuid = kernel::LiveXuid(user);
        const int32_t slot = xuid && Hosting(party) ? MemberOf(party, xuid) : -1;
        if (slot < 0 || uint32_t(slot) == uint32_t(*GuestPtr<be32>(party + T_PARTY_HOST))) return;
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

    // A player who joins is put in the party before the title has read his
    // stats in, so the details AddLocalPlayer takes from them -- his rank
    // among them -- are a player's with none. Each frame they are what his
    // stats say now, the party told when they change.
    void KeepDetails(PPCContext& ctx, uint8_t* base)
    {
        auto read = [&](uint32_t user, auto reader, auto table) {
            const PPCContext saved = ctx;
            ctx.r1.u64 = (ctx.r1.u32 - 0x100) & ~0xFull;
            ctx.r3.u64 = user;
            reader(ctx, base);
            if (table) table(ctx, base);
            const uint32_t value = ctx.r3.u32;
            ctx = saved;
            return value;
        };
        using Fn = void (*)(PPCContext&, uint8_t*);
        for (uint32_t user = 1; user < signin::kUsers; user++)
        {
            if (!g_joined[user]) continue;
            const uint32_t want[4][2] = {
                { 124, read(user, Fn(GUEST_FUNC(T_Member_Detail124)), Fn(nullptr)) },
                { 148, read(user, Fn(GUEST_FUNC(T_Member_Stat148)), Fn(GUEST_FUNC(T_Member_Table148))) },
                { 152, read(user, Fn(GUEST_FUNC(T_Member_Stat152)), Fn(GUEST_FUNC(T_Member_Table152))) },
                { 156, read(user, Fn(GUEST_FUNC(T_Member_Stat156)), Fn(GUEST_FUNC(T_Member_Table156))) },
            };
            for (uint32_t party : kParties)
            {
                const int32_t slot = Hosting(party) ? MemberOf(party, kernel::LiveXuid(user)) : -1;
                if (slot < 0) continue;
                bool changed = false;
                for (const auto& field : want)
                {
                    auto at = GuestPtr<be32>(Member(party, uint32_t(slot)) + field[0]);
                    if (uint32_t(*at) != field[1])
                    {
                        *at = field[1];
                        changed = true;
                    }
                }
                if (!changed) continue;
                const PPCContext saved = ctx;
                ctx.r1.u64 = (ctx.r1.u32 - 0x100) & ~0xFull;
                ctx.r3.u64 = party;
                ctx.r4.u64 = uint32_t(slot);
                GUEST_FUNC(T_Party_MemberChanged)(ctx, base);
                ctx = saved;
                LOGI("splitlive: controller %u's details in party %08x are his stats' now (%u %u %u)", user, party,
                     want[1][1], want[2][1], want[3][1]);
            }
        }
    }

    // In the Xbox LIVE menus, in a party, with no game joined.
    bool InLobby(const State& s)
    {
        if (s.connection[0] != 0) return false;
        for (uint32_t party : kParties)
            if (Hosting(party) || Active(party)) return true;
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
        // makeLocalClientActive at a controller that has its client already is
        // split screen's sign-in finishing, which opens its game setup: then
        // the client is only made active again.
        if (int32_t(*GuestPtr<be32>(T_DATA_ControllerClient + 4 * user)) < 0)
            Run(ctx, base, user, "makeLocalClientActive");
        else
            *GuestPtr<uint8_t>(T_DATA_ClientUIActive + T_CONN_STATE_STRIDE * user) = 1;
        // The lobby's join sound, which AddLocalPlayer does not play (leaving,
        // RemoveMember plays the other).
        bool sound = false;
        for (uint32_t party : kParties)
            if (AddToParty(ctx, base, party, user) && *GuestPtr<be32>(party + T_PARTY_TAKING_MEMBERS) && Active(party))
                sound = true;
#ifdef T_UI_PlayerJoinSound
        if (sound)
        {
            const PPCContext saved = ctx;
            ctx.r1.u64 = (ctx.r1.u32 - 0x100) & ~0xFull;
            ctx.r3.u64 = 0;
            GUEST_FUNC(T_UI_PlayerJoinSound)(ctx, base);
            ctx = saved;
        }
#endif
    }

    void SendLeave(PPCContext& ctx, uint8_t* base, uint32_t party, uint64_t xuid);

    void Leave(PPCContext& ctx, uint8_t* base, uint32_t user)
    {
        g_joined[user] = false;
        for (uint32_t party : kParties) RemoveFromParty(ctx, base, party, user);
        // In a party another machine hosts, the host is told.
        for (uint32_t party : kParties)
            if (!Hosting(party) && Active(party) && MemberOf(party, kernel::LiveXuid(user)) >= 0)
                SendLeave(ctx, base, party, kernel::LiveXuid(user));
        Run(ctx, base, user, "makeLocalClientInactive");
        // Out of the game, out of his profile: START signs in again, as anyone.
        // Signing in made his controller the menus'; were it still, the title
        // would take his signing out for the first player's.
        *GuestPtr<be32>(T_DATA_PrimaryController) = uint32_t(kFirst);
        *GuestPtr<be32>(T_DATA_MenuController) = uint32_t(kFirst);
        signin::SignOut(user);
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

namespace
{
    // What this machine last sent the host of each party it does not host
    // with its first player's memberJoin, for the players beside him.
    struct MemberJoin
    {
        bool sent = false;
        uint64_t address = 0, address2 = 0;   // as the call takes them
        std::string token;
    };
    MemberJoin g_memberJoin[2];
    std::chrono::steady_clock::time_point g_sentAt[2][signin::kUsers];
    bool g_sending = false;
    constexpr auto kAgain = std::chrono::seconds(3);

    int PartyIndex(uint32_t party) { return party == T_DATA_Party0 ? 0 : party == T_DATA_Party1 ? 1 : -1; }

    // The player at `user` to the host of `party`, with the title's memberJoin
    // as the first player's went: his own XUID, name, clan tag and profile.
    void SendMemberJoin(PPCContext& ctx, uint8_t* base, uint32_t party, uint32_t user)
    {
        const MemberJoin& sent = g_memberJoin[PartyIndex(party)];
        const PPCContext saved = ctx;
        const uint32_t sp = (ctx.r1.u32 - 0x800) & ~0xFu;
        const uint32_t token = sp + 0x400;
        std::snprintf(GuestPtr<char>(token), 256, "%s", sent.token.c_str());
        ctx.r1.u64 = sp;
        ctx.r3.u64 = user;
        GUEST_FUNC(T_Live_ProfileName)(ctx, base);
        const uint32_t name = ctx.r3.u32;
        ctx.r3.u64 = user;
        GUEST_FUNC(T_Party_ClanTag)(ctx, base);
        const uint32_t clan = ctx.r3.u32;
        ctx.r3.u64 = user;
        GUEST_FUNC(T_Live_Xuid)(ctx, base);
        const uint64_t xuid = ctx.r3.u64;
        ctx.r1.u64 = sp;
        ctx.r3.u64 = party;
        ctx.r4.u64 = user;
        ctx.r5.u64 = sent.address;
        ctx.r6.u64 = sent.address2;
        ctx.r7.u64 = token;
        ctx.r8.u64 = xuid;
        ctx.r9.u64 = name;
        ctx.r10.u64 = clan;
        // The call also marks the party as joining (+5288 to +5311), as the
        // first player's memberJoin does. This machine is in the party already,
        // and a party that thinks it is joining sends joinParty again, on which
        // the host drops every member of this machine.
        uint8_t joining[24];
        std::memcpy(joining, GuestPtr<uint8_t>(party + 5288), sizeof joining);
        g_sending = true;
        GUEST_FUNC(T_Party_SendMemberJoin)(ctx, base);
        g_sending = false;
        std::memcpy(GuestPtr<uint8_t>(party + 5288), joining, sizeof joining);
        ctx = saved;
        LOGI("splitlive: controller %u (\"%s\", xuid %016llx) sent to the host of party %08x (memberJoin)", user,
             GuestAnsi(name).c_str(), (unsigned long long)xuid, party);
    }

    // To the host of `party`, the title's goodbye ("dis") to the address the
    // first player's memberJoin went to: with a XUID, for that player of this
    // machine beside the first (a host without this switch would take it for
    // the machine's); without, for the machine.
    void SendLeave(PPCContext& ctx, uint8_t* base, uint32_t party, uint64_t xuid)
    {
        const int index = PartyIndex(party);
        if (index < 0 || !g_memberJoin[index].sent) return;
        const MemberJoin& sent = g_memberJoin[index];
        const PPCContext saved = ctx;
        const uint32_t sp = (ctx.r1.u32 - 0x400) & ~0xFu;
        const uint32_t address = sp + 0x100, text = sp + 0x120;
        *GuestPtr<be64>(address) = sent.address;
        *GuestPtr<be32>(address + 8) = uint32_t(sent.address2 >> 32);
        const int32_t number = int32_t(*GuestPtr<be32>(party + T_PARTY_INDEX));
        if (xuid)
            std::snprintf(GuestPtr<char>(text), 64, "%ddis %016llx", number, (unsigned long long)xuid);
        else
            std::snprintf(GuestPtr<char>(text), 64, "%ddis", number);
        ctx.r1.u64 = sp;
        ctx.r3.u64 = address;
        ctx.r4.u64 = text;
        GUEST_FUNC(T_Party_SendText)(ctx, base);
        ctx = saved;
        LOGI("splitlive: told the host of party %08x: \"%s\"", party, GuestPtr<char>(text));
    }

    // The program ending (Goodbye): asked from its own thread, done on the
    // title's next frame.
    std::atomic<bool> g_goodbye{ false }, g_saidGoodbye{ false };

    void SayGoodbye(PPCContext& ctx, uint8_t* base)
    {
        for (uint32_t party : kParties)
        {
            if (Hosting(party) || !Active(party)) continue;
            for (uint32_t user = 1; user < signin::kUsers; user++)
                if (g_joined[user] && MemberOf(party, kernel::LiveXuid(user)) >= 0)
                    SendLeave(ctx, base, party, kernel::LiveXuid(user));
            SendLeave(ctx, base, party, 0);
        }
    }

    // A controller's stats are read in (and, with MW2_UNLOCK_ALL, changed: that
    // is done the frame they are read), as of a frame before this one.
    bool StatsIn(uint32_t user)
    {
        static bool seen[signin::kUsers]{};
        const bool in = *GuestPtr<uint8_t>(T_DATA_Stats + user * T_STATS_STRIDE + T_STATS_READ) != 0;
        const bool was = seen[user];
        seen[user] = in;
        return in && was;
    }

    // In a party another machine hosts: the players in the game beside the
    // first are sent to the host once the party lists the first and their
    // stats are in (memberJoin carries their rank), and again every few
    // seconds until it lists them.
    void SendJoined(PPCContext& ctx, uint8_t* base)
    {
        const auto now = std::chrono::steady_clock::now();
        bool statsIn[signin::kUsers]{};
        for (uint32_t user = 1; user < signin::kUsers; user++) statsIn[user] = StatsIn(user);
        for (uint32_t party : kParties)
        {
            const int index = PartyIndex(party);
            if (!Active(party)) g_memberJoin[index].sent = false;
            if (Hosting(party) || !Active(party) || !g_memberJoin[index].sent) continue;
            if (MemberOf(party, kernel::LiveXuid(kFirst)) < 0) continue;
            for (uint32_t user = 1; user < signin::kUsers; user++)
            {
                const uint64_t xuid = kernel::LiveXuid(user);
                if (!g_joined[user] || !xuid || !statsIn[user] || MemberOf(party, xuid) >= 0) continue;
                if (now - g_sentAt[index][user] < kAgain) continue;
                g_sentAt[index][user] = now;
                SendMemberJoin(ctx, base, party, user);
            }
        }
    }

    // Hosting: another machine's players beside its first are in each party
    // this machine hosts that has their first player, the way they are in the
    // one they joined.
    void CopyBeside(PPCContext& ctx, uint8_t* base)
    {
        if (g_beside.empty()) return;
        static std::map<std::pair<uint32_t, uint64_t>, std::chrono::steady_clock::time_point> tried;
        const auto now = std::chrono::steady_clock::now();
        for (uint32_t party : kParties)
        {
            if (!Hosting(party)) continue;
            for (uint32_t from : kParties)
            {
                if (from == party || !Hosting(from)) continue;
                for (uint32_t i = 0; i < T_SESSION_SLOTS; i++)
                {
                    if (*GuestPtr<uint8_t>(Member(from, i)) < kInParty) continue;
                    const uint64_t xuid = XuidAt(from, i);
                    if (!g_beside.count(xuid) || MemberOf(party, xuid) >= 0) continue;
                    const int32_t first = FirstAt(from, i);
                    if (first < 0 || MemberOf(party, XuidAt(from, uint32_t(first))) < 0) continue;
                    auto& when = tried[{ party, xuid }];
                    if (now - when < kAgain) continue;
                    when = now;
                    CopyToParty(ctx, base, party, from, i);
                }
            }
        }
    }
}

#ifdef T_Party_FindMemberByAddr
PPC_FUNC_IMPL(GUEST_ORIG(T_Party_FindMemberByAddr));

namespace
{
    // A player beside his machine's first being added (memberJoin): nobody is
    // at his machine's address until he is, or the handler takes his message
    // for the first player's details and adds nobody.
    struct Adding
    {
        bool on = false;
        uint32_t party = 0;
        uint64_t xuid = 0, address = 0, address2 = 0;
    } g_adding;

    // Past a machine's players beside its first, to the next member at the
    // address. With nobody else there, another machine's player beside its
    // first is the one, so that a machine leaving takes him along.
    void PassBeside(PPCContext& ctx, uint8_t* base, const PPCContext& call)
    {
        const PPCContext found = ctx;
        const bool other = Beside(call.r3.u32, found.r3.s32) && g_beside.count(XuidAt(call.r3.u32, found.r3.u32));
        while (Beside(call.r3.u32, ctx.r3.s32))
        {
            const int32_t past = ctx.r3.s32;
            ctx = call;
            ctx.r6.u64 = uint32_t(past + 1);
            if (past + 1 < int32_t(T_SESSION_SLOTS)) GUEST_ORIG(T_Party_FindMemberByAddr)(ctx, base);
            if (past + 1 >= int32_t(T_SESSION_SLOTS) || ctx.r3.s32 < 0 || ctx.r3.s32 <= past)   // the fallback walk found him again
            {
                ctx = found;
                if (!other) ctx.r3.s64 = -1;
                return;
            }
        }
    }
}

GUEST_HOOK(T_Party_FindMemberByAddr)
{
    const PPCContext call = ctx;
    if (g_adding.on && call.r3.u32 == g_adding.party && call.r4.u64 == g_adding.address &&
        (call.r5.u64 >> 32) == (g_adding.address2 >> 32) && MemberOf(g_adding.party, g_adding.xuid) < 0)
    {
        ctx.r3.s64 = -1;
        return;
    }
    GUEST_ORIG(T_Party_FindMemberByAddr)(ctx, base);
    if (!splitlive::On()) return;
    PassBeside(ctx, base, call);
    // The host's machine's other players, as anyone sees them: what arrives
    // from the host's address is the host's (partystate names him, and a
    // member found there that is not him is taken for a new host).
    const uint32_t party = call.r3.u32;
    const int32_t found = ctx.r3.s32, host = int32_t(*GuestPtr<be32>(party + T_PARTY_HOST));
    if (found < 0 || host < 0 || host >= int32_t(T_SESSION_SLOTS) || found == host) return;
    if (*GuestPtr<uint8_t>(Member(party, uint32_t(host))) < kInParty) return;
    const uint32_t a = AddressOf(party, uint32_t(found)), b = AddressOf(party, uint32_t(host));
    if (a && b && std::memcmp(GuestPtr<uint8_t>(a), GuestPtr<uint8_t>(b), 12) == 0) ctx.r3.s64 = host;
}
#endif

#ifdef T_Party_RefreshLocalMembers
// The title gives every member signed in here the name of the controller it
// refreshes them for (each frame): the other players here are out of the
// party for the call, as status 2, which it passes over, and keep their own.
GUEST_HOOK(T_Party_RefreshLocalMembers)
{
    const uint32_t party = ctx.r4.u32;
    if (!splitlive::On())
    {
        GUEST_ORIG(T_Party_RefreshLocalMembers)(ctx, base);
        return;
    }
    const uint64_t own = ctx.r3.u32 < signin::kUsers ? kernel::LiveXuid(ctx.r3.u32) : 0;
    uint8_t status[T_SESSION_SLOTS]{};
    for (uint32_t i = 0; i < T_SESSION_SLOTS; i++)
    {
        uint8_t& s = *GuestPtr<uint8_t>(Member(party, i));
        const uint64_t xuid = XuidAt(party, i);
        if (s < kInParty || !xuid || xuid == own) continue;
        for (uint32_t user = 0; user < signin::kUsers; user++)
            if (xuid == kernel::LiveXuid(user))
            {
                status[i] = s;
                s = 2;
                break;
            }
    }
    GUEST_ORIG(T_Party_RefreshLocalMembers)(ctx, base);
    for (uint32_t i = 0; i < T_SESSION_SLOTS; i++)
        if (status[i]) *GuestPtr<uint8_t>(Member(party, i)) = status[i];
}
#endif

#ifdef T_UI_IsItemUnlocked
// The menus ask isItemUnlocked of the first player before a game, which says
// locked while the controllers are routed for split screen: for the call they
// are not, and the first player's unlocks answer.
GUEST_HOOK(T_UI_IsItemUnlocked)
{
    auto multi = GuestPtr<be32>(T_DATA_MultiGamepads);
    const uint32_t client = ctx.r3.u32;
    const bool menus = client < kClients && int32_t(*GuestPtr<be32>(T_DATA_ClientConnState + T_CONN_STATE_STRIDE * client)) < 5;
    const uint32_t was = *multi;
    if (splitlive::On() && was && menus) *multi = 0;
    GUEST_ORIG(T_UI_IsItemUnlocked)(ctx, base);
    if (splitlive::On() && was && menus) *multi = was;
}
#endif

#ifdef T_Party_SendMemberJoin
// What the first player's memberJoin went with, for the players beside him.
GUEST_HOOK(T_Party_SendMemberJoin)
{
    const int index = PartyIndex(ctx.r3.u32);
    if (splitlive::On() && !g_sending && index >= 0)
    {
        MemberJoin& sent = g_memberJoin[index];
        sent.sent = true;
        sent.address = ctx.r5.u64;
        sent.address2 = ctx.r6.u64;
        sent.token = GuestAnsi(ctx.r7.u32);
        for (auto& at : g_sentAt[index]) at = {};
    }
    GUEST_ORIG(T_Party_SendMemberJoin)(ctx, base);
}
#endif

#ifdef T_PartyHost_HandleMemberJoin
namespace
{
    // argv[i] of the command being handled, or "".
    std::string Argv(uint32_t i)
    {
        const uint32_t args = T_DATA_CmdArgs;
        const uint32_t nest = *GuestPtr<be32>(args);
        if (int32_t(*GuestPtr<be32>(args + 68 + nest * 4)) <= int32_t(i)) return {};
        const uint32_t argv = *GuestPtr<be32>(args + 100 + nest * 4);
        return GuestAnsi(*GuestPtr<be32>(argv + 4 * i));
    }
}

// memberJoin (argv[2] the XUID) from a machine whose first player is in the
// party, for another XUID: one of its players beside the first. The handler
// adds him as it adds a machine; it is told it takes members while it does.
GUEST_HOOK(T_PartyHost_HandleMemberJoin)
{
    const uint32_t party = ctx.r3.u32;
    const uint64_t xuid = splitlive::On() && Hosting(party) ? std::strtoull(Argv(2).c_str(), nullptr, 16) : 0;
    if (!xuid)
    {
        GUEST_ORIG(T_PartyHost_HandleMemberJoin)(ctx, base);
        return;
    }
    // In already: the handler, finding his machine's first player at the
    // address, would add him again.
    if (g_beside.count(xuid) && MemberOf(party, xuid) >= 0) return;
    const PPCContext call = ctx;
    ctx.r3.u64 = party;
    ctx.r4.u64 = call.r5.u64;
    ctx.r5.u64 = call.r6.u64;
    ctx.r6.u64 = 0;
    GUEST_FUNC(T_Party_FindMemberByAddr)(ctx, base);
    const int32_t at = ctx.r3.s32;
    ctx = call;
    if (at < 0 || *GuestPtr<uint8_t>(Member(party, uint32_t(at))) < kInParty || XuidAt(party, uint32_t(at)) == xuid)
    {
        GUEST_ORIG(T_PartyHost_HandleMemberJoin)(ctx, base);
        return;
    }
    g_beside[xuid] = XuidAt(party, uint32_t(at));
    // His machine's joinParty reserved a slot for its first player alone: one
    // more, under the challenge he brings, as joinParty reserves them.
    const int32_t slot = FreeSlot(party);
    if (slot >= 0)
    {
        const uint32_t sp = (call.r1.u32 - 0x400) & ~0xFu;
        const uint32_t address = sp + 0x200, challenge = sp + 0x220;
        *GuestPtr<be64>(address) = call.r5.u64;
        *GuestPtr<be32>(address + 8) = uint32_t(call.r6.u64 >> 32);
        std::snprintf(GuestPtr<char>(challenge), 8, "%.5s", Argv(1).c_str());
        ctx.r1.u64 = sp;
        ctx.r3.u64 = party;
        ctx.r4.u64 = uint32_t(slot);
        ctx.r5.u64 = address;
        ctx.r6.u64 = challenge;
        GUEST_FUNC(T_PartyHost_ReserveSlot)(ctx, base);
        ctx = call;
    }
    char first[33]{};
    std::memcpy(first, GuestPtr<char>(Member(party, uint32_t(at)) + T_MEMBER_NAME), 32);
    auto taking = GuestPtr<be32>(party + T_PARTY_TAKING_MEMBERS);
    const uint32_t was = *taking;
    *taking = 1;
    g_adding = { true, party, xuid, call.r5.u64, call.r6.u64 };
    GUEST_ORIG(T_PartyHost_HandleMemberJoin)(ctx, base);
    g_adding = {};
    if (!was) *taking = 0;
    LOGI("splitlive: memberJoin for xuid %016llx from the machine of party %08x's member %d (\"%s\"): %s",
         (unsigned long long)xuid, party, at, first, MemberOf(party, xuid) >= 0 ? "added beside him" : "NOT added");
}
#endif

#ifdef T_PartyHost_HandleDisconnect
// dis (the title's goodbye) with a XUID: one player of the sender's machine
// beside its first leaves, and only he is removed, from each party this
// machine hosts. Without one it is the machine leaving, as the title has it.
GUEST_HOOK(T_PartyHost_HandleDisconnect)
{
    const uint32_t party = ctx.r3.u32;
    const uint64_t xuid = splitlive::On() && Hosting(party) ? std::strtoull(Argv(1).c_str(), nullptr, 16) : 0;
    if (!xuid)
    {
        GUEST_ORIG(T_PartyHost_HandleDisconnect)(ctx, base);
        return;
    }
    const uint64_t from = ctx.r5.u64;   // the sender's address type and IP
    for (uint32_t each : kParties)
    {
        const int32_t slot = Hosting(each) && g_beside.count(xuid) ? MemberOf(each, xuid) : -1;
        const uint32_t at = slot >= 0 ? AddressOf(each, uint32_t(slot)) : 0;
        if (!at || uint64_t(*GuestPtr<be64>(at)) != from) continue;
        char name[33]{};
        std::memcpy(name, GuestPtr<char>(Member(each, uint32_t(slot)) + T_MEMBER_NAME), 32);
        const PPCContext saved = ctx;
        ctx.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFull;
        ctx.r3.u64 = each;
        ctx.r4.u64 = uint32_t(slot);
        ctx.r5.u64 = 0;
        ctx.r6.u64 = T_DATA_EmptyString;
        GUEST_FUNC(T_PartyHost_RemoveMember)(ctx, base);
        ctx = saved;
        LOGI("splitlive: \"%s\" (xuid %016llx) left party %08x (slot %d): his machine said so", name,
             (unsigned long long)xuid, each, slot);
    }
    if (MemberOf(T_DATA_Party0, xuid) < 0 && MemberOf(T_DATA_Party1, xuid) < 0) g_beside.erase(xuid);
}
#endif

#ifdef T_PartyHost_TimeoutMembers
// Another machine's player beside its first is heard from whenever the first
// is: what the machine sends is the first player's.
GUEST_HOOK(T_PartyHost_TimeoutMembers)
{
    const uint32_t party = ctx.r3.u32;
    if (splitlive::On() && !g_beside.empty())
        for (uint32_t i = 0; i < T_SESSION_SLOTS; i++)
        {
            if (*GuestPtr<uint8_t>(Member(party, i)) < 2 || !g_beside.count(XuidAt(party, i))) continue;
            const int32_t first = FirstAt(party, i);
            if (first < 0)
            {
                // His machine's first player has left (or timed out): he goes too.
                char name[33]{};
                std::memcpy(name, GuestPtr<char>(Member(party, i) + T_MEMBER_NAME), 32);
                const PPCContext saved = ctx;
                ctx.r1.u64 = (ctx.r1.u32 - 0x400) & ~0xFull;
                ctx.r3.u64 = party;
                ctx.r4.u64 = i;
                ctx.r5.u64 = 0;
                ctx.r6.u64 = T_DATA_EmptyString;
                GUEST_FUNC(T_PartyHost_RemoveMember)(ctx, base);
                ctx = saved;
                LOGI("splitlive: \"%s\" left party %08x (slot %u) with his machine's first player", name, party, i);
                continue;
            }
            auto heard = GuestPtr<be32>(Member(party, i) + T_MEMBER_HEARD);
            const uint32_t his = *GuestPtr<be32>(Member(party, uint32_t(first)) + T_MEMBER_HEARD);
            if (int32_t(his - uint32_t(*heard)) > 0) *heard = his;
        }
    GUEST_ORIG(T_PartyHost_TimeoutMembers)(ctx, base);
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

#ifdef T_CL_ConnectFromParty
// Joining another machine's game, the party's "go" calls CL_ConnectFromParty
// for each active local client, and each call disconnects every active one,
// gives each the server's address and starts the one it was called for
// (connection state 3): only the last went in. So the first call starts them
// all, and the calls for the others, already on their way in, are passed over.
GUEST_HOOK(T_CL_ConnectFromParty)
{
    const uint32_t client = ctx.r3.u32;
    auto state = [](uint32_t c) { return GuestPtr<be32>(T_DATA_ClientConnState + T_CONN_STATE_STRIDE * c); };
    auto active = [](uint32_t c) { return *GuestPtr<uint8_t>(T_DATA_ClientUIActive + T_CONN_STATE_STRIDE * c) != 0; };
    if (!splitlive::On() || client >= kClients)
    {
        GUEST_ORIG(T_CL_ConnectFromParty)(ctx, base);
        return;
    }
    if (client > 0 && int32_t(*state(client)) > 0)
    {
        LOGI("splitlive: local client %u is connecting already (CL_ConnectFromParty passed over)", client);
        return;
    }
    GUEST_ORIG(T_CL_ConnectFromParty)(ctx, base);
    if (Bool(T_DATA_SvRunningDvar)) return;   // this machine's own game: its clients come in as it loads
    constexpr uint32_t kChallenging = 3;
    for (uint32_t other = 0; other < kClients; other++)
        if (other != client && active(other) && int32_t(*state(other)) == 0)
        {
            *state(other) = kChallenging;
            LOGI("splitlive: local client %u connecting beside local client %u", other, client);
        }
}
#endif

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

#if defined(T_UI_MenuPaint) && defined(T_R_AddCmdDrawText) && defined(T_R_DrawStretchPic)
namespace
{
    // A pad was read at `user` within the last second: one is connected there.
    bool Connected(uint32_t user)
    {
        std::lock_guard g(g_lock);
        return std::chrono::steady_clock::now() - g_readAt[user] < std::chrono::seconds(1);
    }

    // One line of text, its right end at `right`, its baseline at `y`.
    void DrawLine(PPCContext& ctx, uint8_t* base, uint32_t font, const char* text, float right, float y, float scale,
                  const float (&colour)[4])
    {
        const PPCContext saved = ctx;
        const uint32_t sp = (ctx.r1.u32 - 0x400) & ~0xFu;
        const uint32_t string = sp + 0x200, rgba = sp + 0x300;
        std::snprintf(GuestPtr<char>(string), 128, "%s", text);
        for (int i = 0; i < 4; i++)
        {
            uint32_t bits;
            std::memcpy(&bits, &colour[i], 4);
            *GuestPtr<be32>(rgba + 4 * i) = bits;
        }
        ctx.r1.u64 = sp;
        ctx.r3.u64 = string;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = font;
        GUEST_FUNC(T_R_TextWidth)(ctx, base);
        const float width = float(ctx.r3.s32) * scale;
        ctx.r1.u64 = sp;
        ctx.r3.u64 = string;
        ctx.r4.u64 = 128;
        ctx.r5.u64 = font;
        ctx.f1.f64 = right - width;
        ctx.f2.f64 = y;
        ctx.f3.f64 = scale;
        ctx.f4.f64 = scale;
        ctx.f5.f64 = 0.0;
        *GuestPtr<be32>(sp + 84) = rgba;
        *GuestPtr<be32>(sp + 92) = 3;   // shadowed
        GUEST_FUNC(T_R_AddCmdDrawText)(ctx, base);
        ctx = saved;
    }

    // A material by name, or 0.
    uint32_t MaterialNamed(PPCContext& ctx, uint8_t* base, const char* name)
    {
        const PPCContext saved = ctx;
        const uint32_t sp = (ctx.r1.u32 - 0x200) & ~0xFu;
        std::snprintf(GuestPtr<char>(sp + 0x100), 64, "%s", name);
        ctx.r1.u64 = sp;
        ctx.r3.u64 = sp + 0x100;
        ctx.r4.u64 = 3;
        GUEST_FUNC(T_Material_RegisterHandle)(ctx, base);
        const uint32_t material = ctx.r3.u32;
        ctx = saved;
        return material;
    }

    void DrawPicture(PPCContext& ctx, uint8_t* base, uint32_t material, float x, float y, float w, float h)
    {
        const PPCContext saved = ctx;
        const uint32_t sp = (ctx.r1.u32 - 0x400) & ~0xFu;
        const uint32_t rgba = sp + 0x300;
        for (int i = 0; i < 4; i++) *GuestPtr<be32>(rgba + 4 * i) = 0x3F800000u;   // white, opaque
        ctx.r1.u64 = sp;
        ctx.f1.f64 = x;
        ctx.f2.f64 = y;
        ctx.f3.f64 = w;
        ctx.f4.f64 = h;
        ctx.f5.f64 = 0.0;
        ctx.f6.f64 = 0.0;
        ctx.f7.f64 = 1.0;
        ctx.f8.f64 = 1.0;
        *GuestPtr<be32>(sp + 84) = rgba;
        *GuestPtr<be32>(sp + 92) = material;
        GUEST_FUNC(T_R_DrawStretchPic)(ctx, base);
        ctx = saved;
    }

    // With more than one player of this machine in the lobby: a controller
    // left of each one's row of the lobby's list, with his controller's
    // number. The list is the game lobby's party (the second, or the first
    // without it) in slot order, its rows where the private match lobby draws
    // them in a 1280x720 frame, scaled to the frame there is.
    void Controllers(PPCContext& ctx, uint8_t* base, uint32_t font, int32_t pixels, float width, float height)
    {
        bool beside = false;
        for (uint32_t user = 1; user < signin::kUsers; user++) beside = beside || g_joined[user];
        if (!beside) return;
        const uint32_t party = Active(T_DATA_Party1) || Hosting(T_DATA_Party1) ? T_DATA_Party1 : T_DATA_Party0;
        const uint32_t pad = MaterialNamed(ctx, base, "xenon_controller_top");
        if (!pad) return;
        const float sx = width / 1280.0f, sy = height / 720.0f;
        const float rowLeft = 807.0f * sx, firstRow = 111.0f * sy, rowStep = 30.0f * sy;
        const float h = 28.0f * sy, w = 42.0f * sy;
        constexpr float kNumber[4] = { 1.0f, 1.0f, 1.0f, 0.95f };
        int row = 0;
        for (uint32_t i = 0; i < T_SESSION_SLOTS; i++)
        {
            if (*GuestPtr<uint8_t>(Member(party, i)) < kInParty) continue;
            const uint64_t xuid = XuidAt(party, i);
            for (uint32_t user = 0; user < signin::kUsers; user++)
                if (xuid && xuid == kernel::LiveXuid(user))
                {
                    const float middle = firstRow + rowStep * float(row);
                    const float x = rowLeft - 8.0f * sx - w;
                    DrawPicture(ctx, base, pad, x, middle - h / 2, w, h);
                    const std::string number = std::to_string(user + 1);
                    DrawLine(ctx, base, font, number.c_str(), x - 4.0f * sx, middle + h * 0.3f, h * 0.8f / float(pixels), kNumber);
                    break;
                }
            row++;
        }
    }

    // In the lobby: at each connected controller nobody plays at, how to join;
    // beside each player who joined, how to leave.
    void Prompts(PPCContext& ctx, uint8_t* base)
    {
        std::vector<std::pair<std::string, bool>> lines;   // text, a player in the game
        for (uint32_t user = 1; user < signin::kUsers; user++)
        {
            if (g_joined[user])
                lines.emplace_back(signin::At(user).name + ": hold BACK to leave", true);
            else if (Connected(user))
                lines.emplace_back("Controller " + std::to_string(user + 1) + ": press START to join", false);
        }
        if (lines.empty()) return;

        const PPCContext saved = ctx;
        ctx.r1.u64 = (ctx.r1.u32 - 0x100) & ~0xFull;
        ctx.r3.u64 = T_DATA_NormalFontName;
        ctx.r4.u64 = 3;
        GUEST_FUNC(T_R_RegisterFont)(ctx, base);
        const uint32_t font = ctx.r3.u32;
        ctx = saved;
        const int32_t pixels = font ? int32_t(*GuestPtr<be32>(font + 4)) : 0;
        if (pixels <= 0) return;

        // The frame's size (vidConfig: width at +8, height at +12).
        const float width = float(uint32_t(*GuestPtr<be32>(T_DATA_VidConfig + 8)));
        const float height = float(uint32_t(*GuestPtr<be32>(T_DATA_VidConfig + 12)));
        Controllers(ctx, base, font, pixels, width, height);
        const float size = height * 0.032f, scale = size / float(pixels);
        const float right = width * 0.93f;
        float y = height * 0.84f - size * 1.25f * float(lines.size() - 1);
        constexpr float kJoin[4] = { 1.0f, 1.0f, 1.0f, 0.9f }, kLeave[4] = { 0.6f, 0.85f, 1.0f, 0.75f };
        for (const auto& [text, playing] : lines)
        {
            DrawLine(ctx, base, font, text.c_str(), right, y, scale, playing ? kLeave : kJoin);
            y += size * 1.25f;
        }
    }
}

// A lobby's menu drawn: the join prompts and the controllers with it, under
// whatever opens above it, as the rest of the lobby is. Not once the local
// clients are set up for a game (there are none in the lobby): the loading
// screen and the game have no lobby to join.
GUEST_HOOK(T_UI_MenuPaint)
{
    const uint32_t menu = ctx.r4.u32;
    GUEST_ORIG(T_UI_MenuPaint)(ctx, base);
    if (!splitlive::On() || !g_lobby || signin::Open() || *GuestPtr<be32>(T_DATA_LocalClientCount) != 0) return;
    const std::string name = menu ? GuestAnsi(*GuestPtr<be32>(menu)) : std::string();
    if (name == "menu_xboxlive" || name == "menu_xboxlive_lobby" || name == "menu_xboxlive_privatelobby")
        Prompts(ctx, base);
}
#endif

#if defined(T_SV_GetChallenge) && defined(T_NET_OutOfBandPrint) && defined(T_CL_ChallengeKey)
namespace
{
    std::string g_challengeKey;   // the key of the getchallenge being answered

    // argv[i] of a tokenized command (T_DATA_CmdArgs' layout), or "".
    std::string Arg(uint32_t args, uint32_t i)
    {
        const uint32_t nest = *GuestPtr<be32>(args);
        if (int32_t(*GuestPtr<be32>(args + 68 + nest * 4)) <= int32_t(i)) return {};
        const uint32_t argv = *GuestPtr<be32>(args + 100 + nest * 4);
        return GuestAnsi(*GuestPtr<be32>(argv + 4 * i));
    }

    // The key a local client asks a challenge with.
    std::string ChallengeKey(PPCContext& ctx, uint8_t* base, uint32_t localClient)
    {
        const PPCContext saved = ctx;
        const uint32_t sp = (ctx.r1.u32 - 0x200) & ~0xFu;
        const uint32_t buffer = sp + 0x100;
        std::memset(GuestPtr<uint8_t>(buffer), 0, 64);
        ctx.r1.u64 = sp;
        ctx.r3.u64 = localClient;
        ctx.r4.u64 = buffer;
        GUEST_FUNC(T_CL_ChallengeKey)(ctx, base);
        ctx = saved;
        return std::string(GuestPtr<char>(buffer), strnlen(GuestPtr<char>(buffer), 63));
    }
}

// The server answers a getchallenge: the answer says whose key it is.
GUEST_HOOK(T_SV_GetChallenge)
{
    if (splitlive::On()) g_challengeKey = Arg(T_DATA_SvCmdArgs, 2);
    GUEST_ORIG(T_SV_GetChallenge)(ctx, base);
    g_challengeKey.clear();
}

// `challengeResponse <challenge>` goes out as `challengeResponse <challenge>
// <key>`; the title's own client reads the challenge alone.
GUEST_HOOK(T_NET_OutOfBandPrint)
{
    const std::string text = GuestAnsi(ctx.r6.u32);
    if (g_challengeKey.empty() || text.rfind("challengeResponse ", 0) != 0)
    {
        GUEST_ORIG(T_NET_OutOfBandPrint)(ctx, base);
        return;
    }
    const std::string tagged = text + " " + g_challengeKey;
    const uint32_t r1 = ctx.r1.u32;
    const uint32_t sp = (r1 - 0x200) & ~0xFu;
    std::snprintf(GuestPtr<char>(sp + 0x100), 0x100, "%s", tagged.c_str());
    ctx.r1.u64 = sp;
    ctx.r6.u64 = sp + 0x100;
    GUEST_ORIG(T_NET_OutOfBandPrint)(ctx, base);
    ctx.r1.u64 = r1;
}

bool splitlive::ForAnotherClient(PPCContext& ctx, uint8_t* base, uint32_t localClient)
{
    if (!On() || Arg(T_DATA_CmdArgs, 0) != "challengeResponse") return false;
    const std::string key = Arg(T_DATA_CmdArgs, 2);
    if (key.empty() || ChallengeKey(ctx, base, localClient) == key) return false;
    // Another local client still asking for its challenge has that key.
    constexpr int32_t kChallenging = 3;
    for (uint32_t client = 0; client < kClients; client++)
        if (client != localClient &&
            int32_t(*GuestPtr<be32>(T_DATA_ClientConnState + T_CONN_STATE_STRIDE * client)) == kChallenging &&
            ChallengeKey(ctx, base, client) == key)
            return true;
    return false;
}
#else
bool splitlive::ForAnotherClient(PPCContext&, uint8_t*, uint32_t) { return false; }
#endif

namespace
{
    thread_local uint32_t t_sending = 0;   // the socket NET_SendPacket was called with
    std::atomic<uint32_t> g_receivedFor{ 0 };
}

#if defined(T_NET_SendPacket) && defined(T_SV_FindClientByAddr)
// Which local client is sending, for the socket underneath to send from its
// port (kernel/net.cpp).
GUEST_HOOK(T_NET_SendPacket)
{
    const uint32_t was = t_sending;
    t_sending = ctx.r3.u32;
    GUEST_ORIG(T_NET_SendPacket)(ctx, base);
    t_sending = was;
}

// The server client at an address: the one at its IP and port, where several
// local clients of one machine are there each at a port of its own.
GUEST_HOOK(T_SV_FindClientByAddr)
{
    const uint64_t address = ctx.r3.u64;
    const uint16_t port = uint16_t(ctx.r4.u64 >> 48);
    GUEST_ORIG(T_SV_FindClientByAddr)(ctx, base);
    const uint32_t found = ctx.r3.u32;
    if (!splitlive::On() || !found || uint16_t(*GuestPtr<be16>(found + T_SV_CLIENT_ADDRESS + 8)) == port) return;
    const uint32_t count = *GuestPtr<be32>(T_DATA_SvClientCount), clients = *GuestPtr<be32>(T_DATA_SvClients);
    for (uint32_t i = 0; i < count && clients; i++)
    {
        const uint32_t client = clients + i * T_SV_CLIENT_SIZE;
        if (*GuestPtr<be32>(client) && uint64_t(*GuestPtr<be64>(client + T_SV_CLIENT_ADDRESS)) == address &&
            uint16_t(*GuestPtr<be16>(client + T_SV_CLIENT_ADDRESS + 8)) == port)
        {
            ctx.r3.u64 = client;
            return;
        }
    }
}

uint32_t splitlive::SendingClient()
{
    return On() && t_sending > 0 && t_sending < kClients ? t_sending : 0;
}

void splitlive::ReceivedFor(uint32_t localClient) { g_receivedFor = localClient; }

bool splitlive::PacketForAnother(uint32_t localClient, uint32_t addressType)
{
    constexpr uint32_t kIp = 4;
    if (!On() || addressType != kIp || localClient >= kClients) return false;
    const uint32_t to = g_receivedFor;
    if (to) return localClient != to;
    // To the machine's own port: not for a local client in a game at its own,
    // unless this machine runs the game.
    return localClient > 0 &&
           int32_t(*GuestPtr<be32>(T_DATA_ClientConnState + T_CONN_STATE_STRIDE * localClient)) >= 3 &&
           !Bool(T_DATA_SvRunningDvar);
}
#else
uint32_t splitlive::SendingClient() { return 0; }
void splitlive::ReceivedFor(uint32_t) {}
bool splitlive::PacketForAnother(uint32_t, uint32_t) { return false; }
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
    g_readAt[user] = std::chrono::steady_clock::now();
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

void splitlive::Goodbye()
{
#if defined(T_Cmd_ExecuteSingleCommand) && defined(T_PartyHost_AddPlayerAtSlot)
    if (!On()) return;
    g_goodbye = true;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (!g_saidGoodbye && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
#endif
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

    if (g_goodbye && !g_saidGoodbye)
    {
        SayGoodbye(ctx, base);
        g_saidGoodbye = true;
    }

    const bool lobby = InLobby(now);
    g_lobby = lobby;
    // Out of Xbox LIVE (no party, no game) for two seconds -- a party joining
    // again is out for a moment -- the players beside the first leave, their
    // controllers given up as split screen gives them up.
    static std::chrono::steady_clock::time_point outSince;
    const auto clock = std::chrono::steady_clock::now();
    if (lobby || now.connection[0] != 0) outSince = {};
    else if (outSince == std::chrono::steady_clock::time_point{}) outSince = clock;
    if (outSince != std::chrono::steady_clock::time_point{} && clock - outSince >= std::chrono::seconds(2))
    {
        bool left = false;
        for (uint32_t user = 1; user < signin::kUsers; user++)
            if (g_joined[user])
            {
                Leave(ctx, base, user);
                left = true;
            }
        // The primary controller alone again, as Xbox LIVE leaves it.
        if (left) *GuestPtr<be32>(T_DATA_MultiGamepads) = 0;
    }
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
                LOGI("splitlive: START on controller %u: players join in an Xbox LIVE lobby", user);
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
    if (lobby)
    {
        Keep(Read());
        KeepDetails(ctx, base);
        SendJoined(ctx, base);
        CopyBeside(ctx, base);
    }
#else
    (void)ctx; (void)base;
#endif
}
