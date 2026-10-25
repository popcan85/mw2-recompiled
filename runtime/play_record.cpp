// The frame rate of real play (play_record.h).
#include <ppc_recomp_shared.h>
#include "play_record.h"
#include "benchmark.h"
#include "env.h"
#include "guest.h"
#include "log.h"
#include "title.h"
#include "gpu/gpu.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#if defined(T_DATA_ClientConnState) && defined(T_DATA_SvRunningDvar) && defined(T_DATA_SvClients) && defined(T_DATA_MapNameDvar)
namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr int32_t kActive = 9;                        // a local client in play
    constexpr auto kSettle = std::chrono::seconds(10);    // the start of a match, its loading behind it
    constexpr double kShortest = 20.0;                    // seconds of play worth a line

    std::mutex g_lock;
    bool g_playing = false;
    Clock::time_point g_since, g_lastAt;
    uint64_t g_lastFrame = 0;
    std::vector<float> g_gaps;   // seconds between frames, once settled
    std::string g_map;
    bool g_hosting = false;
    uint32_t g_local = 0, g_everyone = 0;

    int32_t Connection(uint32_t client)
    {
        return int32_t(*GuestPtr<be32>(T_DATA_ClientConnState + T_CONN_STATE_STRIDE * client));
    }

    std::string MapName()
    {
        const uint32_t dvar = *GuestPtr<be32>(T_DATA_MapNameDvar);
        const uint32_t text = dvar ? uint32_t(*GuestPtr<be32>(dvar + 12)) : 0;
        return text ? GuestAnsi(text) : std::string("?");
    }

    bool Hosting()
    {
        const uint32_t dvar = *GuestPtr<be32>(T_DATA_SvRunningDvar);
        return dvar && *GuestPtr<uint8_t>(dvar + 12);
    }

    uint32_t ServerClients()
    {
        const uint32_t count = *GuestPtr<be32>(T_DATA_SvClientCount), clients = *GuestPtr<be32>(T_DATA_SvClients);
        uint32_t used = 0;
        for (uint32_t i = 0; i < count && i < 64 && clients; i++)
            if (*GuestPtr<be32>(clients + i * T_SV_CLIENT_SIZE)) used++;
        return used;
    }

    std::string Setting(const char* name, const char* otherwise)
    {
        const char* value = env::Text(name);
        return value && *value ? value : otherwise;
    }

    void WriteLocked()
    {
        double seconds = 0;
        for (float gap : g_gaps) seconds += gap;
        if (seconds >= kShortest)
        {
            std::vector<float> sorted = g_gaps;
            std::sort(sorted.begin(), sorted.end());
            const float slow = sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
            const double fps = double(g_gaps.size()) / seconds, low = slow > 0 ? 1.0 / slow : 0;
            char when[32];
            const std::time_t now = std::time(nullptr);
            std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", std::localtime(&now));
            char line[320];
            std::snprintf(line, sizeof line,
                          "%s map=%s hosting=%d local=%u players=%u render=%s scale=%s msaa=%s limit=%s seconds=%.0f fps=%.1f low1=%.1f\n",
                          when, g_map.c_str(), g_hosting ? 1 : 0, g_local, g_everyone, Setting("MW2_RENDER", "1280x720").c_str(),
                          Setting("MW2_SCALE", "1").c_str(),
                          env::Flag("MW2_NO_MSAA") ? "0" : Setting("MW2_MSAA", "2").c_str(), Setting("MW2_FPS_LIMIT", "60").c_str(),
                          seconds, fps, low);
            std::ofstream("performance.txt", std::ios::app) << line;
            LOGI("play record: %s", line);
        }
        g_playing = false;
        g_gaps.clear();
    }
}

void playrecord::Frame(PPCContext&, uint8_t*)
{
    if (benchmark::On()) return;
    static uint64_t seen = ~0ull;
    const uint64_t frame = gpu::FrameCount();
    if (frame == seen) return;
    seen = frame;

    std::lock_guard lock(g_lock);
    const auto now = Clock::now();
    const bool playing = Connection(0) >= kActive;
    if (!playing)
    {
        if (g_playing) WriteLocked();
        return;
    }
    if (!g_playing)
    {
        g_playing = true;
        g_since = now;
        g_gaps.clear();
        g_local = g_everyone = 0;
    }
    if (now - g_since < kSettle)
    {
        g_lastFrame = frame;
        g_lastAt = now;
        return;
    }
    // Who is in it, at its fullest.
    uint32_t local = 0;
    for (uint32_t client = 0; client < 4; client++) local += Connection(client) >= kActive ? 1 : 0;
    g_local = std::max(g_local, local);
    g_hosting = Hosting();
    if (g_hosting) g_everyone = std::max(g_everyone, ServerClients());
    g_map = MapName();
    if (frame > g_lastFrame)
    {
        const float gap = std::chrono::duration<float>(now - g_lastAt).count() / float(frame - g_lastFrame);
        for (uint64_t i = g_lastFrame; i < frame && g_gaps.size() < 4'000'000; i++) g_gaps.push_back(gap);
    }
    g_lastFrame = frame;
    g_lastAt = now;
}

void playrecord::End()
{
    std::lock_guard lock(g_lock);
    if (g_playing) WriteLocked();
}
#else
void playrecord::Frame(PPCContext&, uint8_t*) {}
void playrecord::End() {}
#endif
