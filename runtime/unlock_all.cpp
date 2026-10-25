// MW2_UNLOCK_ALL: every player at the end of rank 70, everything unlocked.
//
// The launcher's PROFILES screen does this to a stats file with the title
// closed (launcher/profile.cpp, RANK and UNLOCK EVERYTHING). Here it is done
// to the stats the title has read in, for each controller: whoever signs in,
// at any controller, on any machine, plays with all of it, and the title
// writes it to his file itself as it saves his stats.
//
// A controller's stats in memory are laid out as the file is (T_DATA_Stats,
// two copies), so the change is the launcher's: `experience` to the end of
// rank 70, each challenge's state one past its last tier and its progress at
// that tier's target, and every title, emblem and killstreak bit. Prestige is
// left as it is. A copy is changed only while it holds stats of the layout
// the change knows (its version and checksum), and only while its experience
// is not the most there is; the stats' CRC is then made again with the
// title's own CRC-32, as its reset makes it, and the stats marked as needing
// to be written, without which the title never saves them. Stats read in
// again, or reset, are changed again on the next frame.
#include <ppc_recomp_shared.h>
#include "unlock_all.h"
#include "env.h"
#include "guest.h"
#include "log.h"
#include "title.h"
#include "../launcher/playerdata_layout.h"

#include <cstdint>

#if defined(T_DATA_Stats) && defined(T_Crc32)
namespace
{
    constexpr uint32_t kControllers = 4, kCopies = 2, kBuffer = 8188;

    uint32_t Get32(uint8_t* stats, uint32_t offset)
    {
        const uint8_t* p = stats + offset;
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }

    void Put32(uint8_t* stats, uint32_t offset, uint32_t value)
    {
        for (int i = 0; i < 4; i++) stats[offset + i] = uint8_t(value >> (8 * i));
    }

    void SetBit(uint8_t* stats, uint32_t offset, uint32_t index) { stats[offset + index / 8] |= uint8_t(1u << (index % 8)); }

    void Unlock(uint8_t* stats)
    {
        Put32(stats, playerdata::kExperience, playerdata::kMaxExperience);
        for (const playerdata::Challenge& challenge : playerdata::kChallenges)
        {
            stats[playerdata::kChallengeState + challenge.index] = uint8_t(challenge.tiers + 1);
            Put32(stats, playerdata::kChallengeProgress + 4 * challenge.index, challenge.target);
        }
        for (uint16_t index : playerdata::kTitles) SetBit(stats, playerdata::kTitleUnlocked, index);
        for (uint16_t index : playerdata::kIcons) SetBit(stats, playerdata::kIconUnlocked, index);
        for (uint16_t index : playerdata::kKillstreaks) SetBit(stats, playerdata::kKillstreakUnlocked, index);
    }
}

void unlockall::Frame(PPCContext& ctx, uint8_t* base)
{
    static const bool on = env::Flag("MW2_UNLOCK_ALL");
    if (!on) return;
    for (uint32_t controller = 0; controller < kControllers; controller++)
        for (uint32_t copy = 0; copy < kCopies; copy++)
        {
            const uint32_t at = T_DATA_Stats + controller * T_STATS_STRIDE + copy * T_STATS_COPY;
            uint8_t* stats = GuestPtr<uint8_t>(at + 4);
            if (Get32(stats, 0) != playerdata::kVersion || Get32(stats, 4) != playerdata::kFormat ||
                Get32(stats, playerdata::kExperience) == playerdata::kMaxExperience)
                continue;
            Unlock(stats);
            const PPCContext saved = ctx;
            ctx.r1.u64 = (ctx.r1.u32 - 0x100) & ~0xFull;
            ctx.r3.u64 = at + 4;
            ctx.r4.u64 = kBuffer;
            ctx.r5.u64 = 0;
            GUEST_FUNC(T_Crc32)(ctx, base);
            *GuestPtr<be32>(at) = ctx.r3.u32;
            ctx = saved;
            // The title writes a controller's stats only when told they changed.
            if (copy == 0 && *GuestPtr<uint8_t>(at + T_STATS_READ)) *GuestPtr<uint8_t>(at + T_STATS_WRITE_NEEDED) = 1;
            LOGI("unlockall: controller %u's stats (copy %u) at the end of rank 70, everything unlocked", controller, copy);
        }
}
#else
void unlockall::Frame(PPCContext&, uint8_t*) {}
#endif
