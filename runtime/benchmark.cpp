// MW2_BENCHMARK: the multiplayer measuring itself (benchmark.h).
#include <ppc_recomp_shared.h>
#include "benchmark.h"
#include "crash.h"
#include "env.h"
#include "guest.h"
#include "log.h"
#include "signin.h"
#include "title.h"
#include "gpu/gpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using Seconds = std::chrono::duration<double>;

    constexpr uint16_t kStart = 0x0010, kA = 0x1000;
    constexpr int16_t kTurn = 18000;   // the right stick, a slow turn past the dead zone

    // The multiplayer's alone: the campaign has no split screen to start.
#if defined(T_DATA_ClientConnState) && defined(T_Cmd_ExecuteSingleCommand)
    constexpr bool kCan = true;
#else
    constexpr bool kCan = false;
#endif

    uint32_t Wanted()
    {
        static const uint32_t players = kCan ? uint32_t(std::clamp<uint64_t>(env::Number("MW2_BENCHMARK", 0), 0, 4)) : 0;
        return players;
    }

    enum class Stage { Boot, Settings, Activate, Start, Loading, Warmup, Measure, Done };

    std::mutex g_lock;   // what the stage tells the controllers
    Stage g_stage = Stage::Boot;
    Clock::time_point g_since = Clock::now();   // the stage's start
    bool g_pressStart = false, g_pressA = false, g_turn = false;
    std::atomic<bool> g_mainSeen{ false };

    std::vector<double> g_frameTimes;   // seconds between the title's frames, measuring
    uint64_t g_lastFrame = 0;
    Clock::time_point g_lastFrameAt;

    double Since() { return Seconds(Clock::now() - g_since).count(); }

    void Enter(Stage stage, const char* what)
    {
        g_stage = stage;
        g_since = Clock::now();
        LOGI("benchmark: %s", what);
    }

    // The command, run as `controller` the way the menus run it.
    void Run(PPCContext& ctx, uint8_t* base, uint32_t controller, const char* command)
    {
#ifdef T_Cmd_ExecuteSingleCommand
        const uint32_t sp = ctx.r1.u32;
        const uint32_t scratch = (sp - 0x200) & ~0xFu;
        std::snprintf(GuestPtr<char>(scratch), 0x100, "%s", command);
        const PPCContext saved = ctx;
        ctx.r1.u64 = (sp - 0x400) & ~0xFull;
        ctx.r3.u64 = controller;
        ctx.r4.u64 = controller;
        ctx.r5.u64 = scratch;
        GUEST_FUNC(T_Cmd_ExecuteSingleCommand)(ctx, base);
        ctx = saved;
        LOGI("benchmark: ran \"%s\" as controller %u", command, controller);
#else
        (void)ctx; (void)base; (void)controller; (void)command;
#endif
    }

    int32_t Connection(uint32_t client)
    {
#ifdef T_DATA_ClientConnState
        return int32_t(*GuestPtr<be32>(T_DATA_ClientConnState + T_CONN_STATE_STRIDE * client));
#else
        (void)client;
        return 0;
#endif
    }

    std::string Setting(const char* name, const char* otherwise)
    {
        const char* value = env::Text(name);
        return value && *value ? value : otherwise;
    }

    // What was measured, to the file the launcher reads, and the end.
    void Finish(const std::string& result)
    {
        const std::string path = Setting("MW2_BENCHMARK_OUT", "benchmark.txt");
        double total = 0;
        for (double t : g_frameTimes) total += t;
        const double fps = total > 0 ? double(g_frameTimes.size()) / total : 0;
        // The slowest hundredth of the frames, as a rate.
        double low = 0;
        if (!g_frameTimes.empty())
        {
            std::vector<double> sorted = g_frameTimes;
            std::sort(sorted.begin(), sorted.end());
            const size_t at = std::min(sorted.size() - 1, sorted.size() * 99 / 100);
            low = sorted[at] > 0 ? 1.0 / sorted[at] : 0;
        }
        std::ofstream out(path, std::ios::trunc);
        out << "result=" << result << "\n"
            << "players=" << Wanted() << "\n"
            << "map=" << Setting("MW2_BENCHMARK_MAP", "mp_terminal") << "\n"
            << "seconds=" << total << "\n"
            << "frames=" << g_frameTimes.size() << "\n"
            << "fps=" << fps << "\n"
            << "low1=" << low << "\n"
            << "render=" << Setting("MW2_RENDER", "1280x720") << "\n"
            << "scale=" << Setting("MW2_SCALE", "1") << "\n"
            << "msaa=" << (env::Flag("MW2_NO_MSAA") ? std::string("0") : Setting("MW2_MSAA", "2")) << "\n"
            << "limit=" << Setting("MW2_FPS_LIMIT", "60") << "\n";
        LOGI("benchmark: %s, %u players, %.1f fps on average, %.1f at the slowest 1%%, over %.1f s (%s)", result.c_str(),
             Wanted(), fps, low, total, path.c_str());
        Enter(Stage::Done, "done");
        crash::RequestExit("the benchmark is done");
    }
}

bool benchmark::On() { return Wanted() > 0; }
uint32_t benchmark::Players() { return Wanted(); }

namespace
{
    // The menus drawn, by name, and when each was last.
    std::mutex g_menuLock;
    std::vector<std::pair<std::string, Clock::time_point>> g_menus;

    // A menu that asks something before play (team, class) drawn in the last
    // half second.
    bool Asking()
    {
        std::lock_guard lock(g_menuLock);
        const auto now = Clock::now();
        for (const auto& [name, at] : g_menus)
            if (now - at < std::chrono::milliseconds(500) &&
                (name.find("team") != std::string::npos || name.find("class") != std::string::npos))
                return true;
        return false;
    }
}

void benchmark::MenuPainted(const char* name)
{
    if (!g_mainSeen && (std::strcmp(name, "main") == 0 || std::strcmp(name, "main_text") == 0))
    {
        g_mainSeen = true;
        LOGI("benchmark: the main menu is up (%s)", name);
    }
    std::lock_guard lock(g_menuLock);
    for (auto& [known, at] : g_menus)
        if (known == name)
        {
            at = Clock::now();
            return;
        }
    g_menus.emplace_back(name, Clock::now());
    LOGI("benchmark: menu %s drawn", name);
}

bool benchmark::Drives(uint32_t user) { return On() && user < Wanted(); }

void benchmark::Drive(uint32_t user, uint16_t& buttons, int16_t axes[4])
{
    if (!Drives(user)) return;
    std::lock_guard lock(g_lock);
    // A press a fifth of a second long, every two seconds (three for START).
    const double t = Since();
    buttons = 0;
    axes[0] = axes[1] = axes[2] = axes[3] = 0;
    if (g_pressStart && user == 0 && std::fmod(t, 3.0) < 0.2) buttons |= kStart;
    if (g_pressA && std::fmod(t, 1.0) < 0.2) buttons |= kA;
    if (g_turn) axes[2] = kTurn;
}

void benchmark::Frame(PPCContext& ctx, uint8_t* base)
{
    if (!On()) return;
    // The title polls every controller a frame: once a frame is enough here.
    static uint64_t seen = ~0ull;
    const uint64_t frame = gpu::FrameCount();
    if (frame == seen) return;
    seen = frame;

    std::lock_guard lock(g_lock);
    const uint32_t players = Wanted();
    const double t = Since();
    switch (g_stage)
    {
    case Stage::Boot:
        // The menus up and settled; START pressed now and then if the title
        // waits for it first.
        g_pressStart = !g_mainSeen && t > 20.0;
        if (g_mainSeen && t > 2.0)
        {
            g_pressStart = false;
            Enter(Stage::Settings, "the split-screen settings");
        }
        else if (t > 90.0) Finish("failed: the main menu never came up");
        break;

    case Stage::Settings:
        // What the main menu's SPLIT SCREEN runs.
        for (const char* command : { "splitscreencontrols", "set systemlink 0", "set splitscreen 1", "set onlinegame 0",
                                     "xblive_rankedmatch 0", "xblive_privatematch 0", "exec default_splitscreen.cfg",
                                     "set ui_mptype 2", "resetSplitscreenSignIn" })
            Run(ctx, base, 0, command);
        for (uint32_t user = 1; user < players; user++) signin::SignInTemporary(user, "Benchmark " + std::to_string(user + 1));
        Enter(Stage::Activate, "players signed in");
        break;

    case Stage::Activate:
        // The title reads the profiles in first.
        if (t < 2.0) break;
        for (uint32_t user = 0; user < players; user++) Run(ctx, base, user, "makeLocalClientActive");
        Enter(Stage::Start, "every player's client active");
        break;

    case Stage::Start:
        if (t < 1.0) break;
        {
            const std::string map = Setting("MW2_BENCHMARK_MAP", "mp_terminal");
            Run(ctx, base, 0, "set ui_gametype war");
            Run(ctx, base, 0, "set g_gametype war");
            Run(ctx, base, 0, ("map " + map).c_str());
        }
        Enter(Stage::Loading, "loading the map");
        break;

    case Stage::Loading:
    {
        // Every player in, and A now and then for what asks before play
        // (team, class).
        static int32_t last[4] = { -1, -1, -1, -1 };
        bool in = true;
        for (uint32_t client = 0; client < players; client++)
        {
            const int32_t state = Connection(client);
            if (state != last[client]) LOGI("benchmark: client %u connection state %d", client, state);
            last[client] = state;
            in = in && state >= 7;
        }
        g_pressA = t > 5.0;
        static double inSince = -1;
        if (!in) inSince = -1;
        else if (inSince < 0) inSince = t;
        if (in && t - inSince > 3.0) Enter(Stage::Warmup, "in play; warming up");
        else if (t > 180.0) Finish("failed: the match never started");
        break;
    }

    case Stage::Warmup:
        // A while a menu asks something, then a few seconds of play.
        {
            static double askedAt = 0;
            const bool asking = Asking();
            if (asking) askedAt = t;
            g_pressA = asking || t < 4.0;
            g_turn = !asking;
            if (t > 40.0)
            {
                Finish("failed: the players never got past the menus before play");
                break;
            }
            if (asking || t < 6.0 || t - askedAt < 4.0) break;
        }
        {
            g_pressA = false;
            g_frameTimes.clear();
            g_lastFrame = frame;
            g_lastFrameAt = Clock::now();
            Enter(Stage::Measure, "measuring");
        }
        break;

    case Stage::Measure:
    {
        const auto now = Clock::now();
        if (frame > g_lastFrame)
        {
            const double gap = Seconds(now - g_lastFrameAt).count() / double(frame - g_lastFrame);
            for (uint64_t i = g_lastFrame; i < frame; i++) g_frameTimes.push_back(gap);
            g_lastFrame = frame;
            g_lastFrameAt = now;
        }
        const double seconds = double(std::clamp<uint64_t>(env::Number("MW2_BENCHMARK_SECONDS", 15), 5, 120));
        if (t >= seconds) Finish("ok");
        break;
    }

    case Stage::Done:
        break;
    }
}
