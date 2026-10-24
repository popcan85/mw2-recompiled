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
// The server then turns the second player away: it lets in a player only
// when a session holds his XUID (SV_DirectConnect, "Party_FindMember() worked
// but Session_FindRegisteredUser() did not"), and the title registers one
// player a machine, the first. So whenever it registers this machine's first
// player in a session, the other active local players are registered there
// too, in free slots and otherwise as he was; and a player made active later
// is registered in the sessions the first is already in.
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
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    constexpr uint16_t kBack = 0x0020, kStart = 0x0010;
    constexpr auto kHold = std::chrono::seconds(1);

    std::mutex g_lock;
    std::chrono::steady_clock::time_point g_backSince[signin::kUsers];
    bool g_backHeld[signin::kUsers]{}, g_backDone[signin::kUsers]{};
    bool g_toggle[signin::kUsers]{};

#ifdef T_Cmd_ExecuteSingleCommand
    constexpr uint32_t kClients = 4;

    struct State
    {
        int32_t multi = 0, primary = 0;
        std::array<int32_t, kClients> client{};
        std::array<uint8_t, kClients> active{};
        std::array<int32_t, kClients> connection{};
        bool operator==(const State&) const = default;
    };

    State Read()
    {
        State s;
        s.multi = *GuestPtr<be32>(T_DATA_MultiGamepads);
        s.primary = *GuestPtr<be32>(T_DATA_PrimaryController);
        for (uint32_t i = 0; i < kClients; i++)
        {
            s.client[i] = *GuestPtr<be32>(T_DATA_ControllerClient + 4 * i);
            s.active[i] = *GuestPtr<uint8_t>(T_DATA_ClientUIActive + T_CONN_STATE_STRIDE * i);
            s.connection[i] = *GuestPtr<be32>(T_DATA_ClientConnState + T_CONN_STATE_STRIDE * i);
        }
        return s;
    }

    void Note(const State& s)
    {
        std::string text;
        char part[96];
        for (uint32_t i = 0; i < kClients; i++)
        {
            std::snprintf(part, sizeof part, " | controller %u -> client %d, client %u %s, connection %d", i,
                          s.client[i], i, s.active[i] ? "active" : "inactive", s.connection[i]);
            text += part;
        }
        LOGI("splitlive: multi gamepads %d, primary controller %d%s", s.multi, s.primary, text.c_str());
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
#endif
}

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
    LOGI("splitlive: local clients set up for Xbox LIVE (server running %u): %u before, %u now, room for 18 players a snapshot",
         running, before, uint32_t(*GuestPtr<be32>(T_DATA_LocalClientCount)));
}

PPC_FUNC_IMPL(GUEST_ORIG(T_Session_RegisterUser));

namespace
{
    // How the first player was put in a session: the arguments again, but for
    // the xuid and the slot.
    struct Registration
    {
        uint32_t controller = 0, session = 0, privateSlot = 0, natType = 0, r9 = 0, r10 = 0;
        uint64_t stacked = 0;
    };
    std::mutex g_registrationLock;
    std::vector<Registration> g_registrations;   // a session each
    bool g_registering = false;

    bool Holds(uint32_t session, uint64_t xuid, int32_t* slot = nullptr)
    {
        for (uint32_t i = 0; i < T_SESSION_SLOTS; i++)
        {
            const uint32_t at = session + T_SESSION_USERS + i * T_SESSION_USER_SIZE;
            if (*GuestPtr<uint8_t>(at) && uint64_t(*GuestPtr<be64>(at + T_SESSION_USER_XUID)) == xuid)
            {
                if (slot) *slot = int32_t(i);
                return true;
            }
        }
        return false;
    }

    // Every active local player beside the first, into the session the first
    // was registered in as `r` says.
    void RegisterLocals(PPCContext& ctx, uint8_t* base, const Registration& r)
    {
        for (uint32_t user = 1; user < signin::kUsers; user++)
        {
            const uint64_t xuid = kernel::LiveXuid(user);
            if (!xuid || !*GuestPtr<uint8_t>(T_DATA_ClientUIActive + T_CONN_STATE_STRIDE * user) ||
                Holds(r.session, xuid))
                continue;
            int32_t slot = -1;
            for (uint32_t i = 0; i < T_SESSION_SLOTS && slot < 0; i++)
                if (!*GuestPtr<uint8_t>(r.session + T_SESSION_USERS + i * T_SESSION_USER_SIZE)) slot = int32_t(i);
            if (slot < 0)
            {
                LOGW("splitlive: no free slot for controller %u in session %08x", user, r.session);
                continue;
            }
            const PPCContext saved = ctx;
            const uint32_t sp = (ctx.r1.u32 - 0x400) & ~0xFu;
            *GuestPtr<be64>(sp + 88) = r.stacked;   // the ninth argument, where the callee reads it
            ctx.r1.u64 = sp;
            ctx.r3.u64 = r.controller;
            ctx.r4.u64 = r.session;
            ctx.r5.u64 = xuid;
            ctx.r6.u64 = r.privateSlot;
            ctx.r7.u64 = uint32_t(slot);
            ctx.r8.u64 = r.natType;
            ctx.r9.u64 = r.r9;
            ctx.r10.u64 = r.r10;
            g_registering = true;
            GUEST_ORIG(T_Session_RegisterUser)(ctx, base);
            g_registering = false;
            ctx = saved;
            LOGI("splitlive: controller %u (xuid %016llx) registered in session %08x at slot %d, beside the first player",
                 user, (unsigned long long)xuid, r.session, slot);
        }
    }

    void RegisterLater(PPCContext& ctx, uint8_t* base)
    {
        std::vector<Registration> known;
        {
            std::lock_guard g(g_registrationLock);
            known = g_registrations;
        }
        const uint64_t first = kernel::LiveXuid(0);
        for (const Registration& r : known)
            if (Holds(r.session, first)) RegisterLocals(ctx, base, r);
    }
}

GUEST_HOOK(T_Session_RegisterUser)
{
    const bool first = splitlive::On() && !g_registering && ctx.r5.u64 == kernel::LiveXuid(0);
    Registration r;
    if (first)
    {
        r.controller = ctx.r3.u32;
        r.session = ctx.r4.u32;
        r.privateSlot = ctx.r6.u32;
        r.natType = ctx.r8.u32;
        r.r9 = ctx.r9.u32;
        r.r10 = ctx.r10.u32;
        r.stacked = *GuestPtr<be64>(ctx.r1.u32 + 88);
    }
    GUEST_ORIG(T_Session_RegisterUser)(ctx, base);
    if (!first) return;
    {
        std::lock_guard g(g_registrationLock);
        std::erase_if(g_registrations, [&](const Registration& known) { return known.session == r.session; });
        g_registrations.push_back(r);
    }
    RegisterLocals(ctx, base, r);
}

GUEST_HOOK(T_SV_InitHunk)
{
    const uint32_t copy = g_realSplitscreen ? uint32_t(*GuestPtr<be32>(T_DATA_SplitscreenDvar)) : 0;
    if (copy) *GuestPtr<be32>(T_DATA_SplitscreenDvar) = g_realSplitscreen;
    GUEST_ORIG(T_SV_InitHunk)(ctx, base);
    if (copy) *GuestPtr<be32>(T_DATA_SplitscreenDvar) = copy;
}
#endif

bool splitlive::On()
{
    static const bool on = env::Flag("MW2_SPLIT_LIVE") && online::Live();
    return on;
}

void splitlive::Input(uint32_t user, uint16_t buttons)
{
    if (!On() || user == 0 || user >= signin::kUsers) return;
    std::lock_guard g(g_lock);
    const bool back = (buttons & (kBack | kStart)) == kBack;
    const auto now = std::chrono::steady_clock::now();
    if (back && !g_backHeld[user]) { g_backSince[user] = now; g_backDone[user] = false; }
    g_backHeld[user] = back;
    if (back && !g_backDone[user] && now - g_backSince[user] >= kHold)
    {
        g_backDone[user] = true;
        if (signin::At(user).signedIn) g_toggle[user] = true;
        else LOGI("splitlive: controller %u held Back, but nobody is signed in there", user);
    }
}

void splitlive::Frame(PPCContext& ctx, uint8_t* base)
{
    if (!On()) return;
#ifdef T_Cmd_ExecuteSingleCommand
    static State last;
    static bool first = true;
    const State now = Read();
    if (first || !(now == last)) Note(now);
    first = false;
    last = now;

    for (uint32_t user = 1; user < signin::kUsers; user++)
    {
        {
            std::lock_guard g(g_lock);
            if (!g_toggle[user]) continue;
            g_toggle[user] = false;
        }
        if (now.active[user])
        {
            Run(ctx, base, user, "makeLocalClientInactive");
            continue;
        }
        if (!now.multi)
        {
            *GuestPtr<be32>(T_DATA_MultiGamepads) = 1;
            LOGI("splitlive: every controller routed to its own client (cl_multi_gamepads_enabled)");
        }
        Run(ctx, base, user, "makeLocalClientActive");
#ifdef T_Session_RegisterUser
        RegisterLater(ctx, base);
#endif
    }
#else
    (void)ctx; (void)base;
#endif
}
