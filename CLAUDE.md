# CLAUDE.md

Fork of PaulCombal/mw2-recompiled (remote `upstream`); ours is popcan85/mw2-recompiled
(remote `origin`). Branch: `split-screen-lan`. GPLv3.

## Goal
Split-screen players on several PCs in one Xbox LIVE private match over the `lan`
service (as Project Reclaimer does for Halo 3). End goal: up to 4 players per PC.
The user tests on Windows PCs with real controllers; Claude does the engineering.

## Rules
- Every behaviour change behind a new `MW2_` switch; default = upstream behaviour.
  Document each in `docs/switches.md` (and `docs/multiplayer.md` where relevant).
- Small, upstreamable changes, upstream's code and doc style.
- Stop at the end of each stage with a plain-language report and Windows test steps.
- Commit at the end of each working stage; push the branch to `origin` only, never upstream.
- Never commit the ISO, the title update or anything under `mw2/`.

## Paths
- Repo: `~/mw2-recompiled` (WSL). Build here, not under `/mnt/c`.
- ISO: `/mnt/c/AI/Projects/mw2/iso/Call of Duty - Modern Warfare 2 (USA, Europe).iso`
- Title update 6 package: `~/mw2-assets/tu6/` (from the launcher's pinned codxenon URL)
- llvm-mingw: `~/toolchains/llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64` (release.yml's version)
- Test builds for the user: `C:\AI\Projects\mw2\builds\<stage>\` with a `launch.bat`
  that sets `MW2_LOG_FILE=C:\AI\Projects\mw2\logs\mw2.log`.
- Logs: `/mnt/c/AI/Projects/mw2/logs/` (`mw2.log` this PC, `mw2-pc2.log`... others).
- PC2 (MARSHMELLOW, 192.168.0.144) shares its `C:\AI\Projects\mw2` as `\\192.168.0.144\mw2`
  (Git Bash: `//192.168.0.144/mw2`): put PC2's builds in `builds/pc2-kit/` there and read
  `logs/mw2-pc2.log` directly. Its launch.bat must set `MW2_LAN_ACCEPT=1` to take invites.

## Build (Windows multiplayer, lan, as the official release)
    cd ~/mw2-recompiled
    ISO="/mnt/c/AI/Projects/mw2/iso/Call of Duty - Modern Warfare 2 (USA, Europe).iso" \
    TU=~/mw2-assets/tu6/TU_10LC20N_0000018000000.00000000001GA WINDOWS=1 TITLE=mp RELEASE=1 ONLINE=lan \
    LLVM_MINGW=~/toolchains/llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64 ./build.sh
Output: `build-win-mp-release/mw2.exe` and `mw2-launcher.exe`. Ship as `mw2-mp.exe`.
A rebuild after runtime-only changes: add `REGENERATE=0`.
Diagnostic build (for `MW2_TRACE_ONLINE`): drop `RELEASE=1` -> `build-win-mp/`.

## Stages
0. Build stock project. 1. `lan.cpp` adapter choice (`MW2_LAN_ADDRESS`, skip 169.254,
prefer private + gateway). 2. `MW2_SPLIT_LIVE=1`: controllers 2-4 signed in to Live with
own XUIDs; 2+2 players across 2 PCs in one private match. 3. Investigate only: 3-4 per PC /
system-link split-screen; report approach, risks, odds.

## Progress
- Setup done (WSL tools, clone, `upstream` remote, branch).
- Stage 0: stock build of upstream b2ebb69 built OK -> `builds\stage0-stock\`
  (`install.bat` installs from the ISO once, `launch.bat` runs it). User confirmed it
  runs like the official release. DONE.
- Stage 1: `lan.cpp` ranks adapters (private+gateway on a card first, link-local last),
  broadcasts to the chosen subnet's broadcast address, `MW2_LAN_ADDRESS=<ip>|first`.
  Verified choice on this PC (Mullvad, Tailscale, Ethernet, vEthernet -> Ethernet) with
  a standalone test exe. Build in `builds\stage1-adapter\` (junctions to stage0-stock's
  game/saves/shaders). Two-PC test passed: private match invite PC1 -> PC2 with Mullvad
  and Tailscale up on PC1 (invitee needs `MW2_LAN_ACCEPT=1`). DONE.
- Stage 2 (in progress, `runtime/split_live.cpp`, docs/multiplayer.md "Split screen on
  Xbox LIVE"): MW2 itself has no online split screen (Live sets splitscreen 0, 1 local
  client, 1 party member/registration per machine). Native route on ONE PC works and the
  user confirmed it: in the Xbox LIVE lobby this PC hosts, START on controller 2 joins
  (sign-in screen first if needed; join waits 1.5 s for the profile), Back held 2 s leaves;
  4 local clients made online (copy of the splitscreen dvar), 18-player snapshot room;
  player 2 added to both hosted parties with PartyHost_AddPlayerAtSlot (shows in lobby);
  Party_FindMemberByAddr skips this PC's extra players; menus kept on controller 0 and
  only joined clients active (the game's sign-in handler makes the new controller primary
  and every client active); START that joins is hidden until released. Private match:
  split screen, both playable, player 2's name right in game.
  Build for testing: `builds\stage2c-diagnostic\` (diagnostic: MW2_ENGINE_LOG=1, LOG_TIME).
  OPEN, next session:
  1. Lobby name flips "Player 2" -> "Reido" ~40 ms after player 2 joins the lobby party
     (party 825B5C18, his slot 0, host slot 7), right after "Session Join completed" for
     him in gameSession. Suspect the join-complete callback sub_823915C0 -> sub_821A6578
     (refreshes the host's member with sub_821A44A0 and syncs linked party +5464 members).
     Find the writer: hook T_I_strncpyz / AddPlayerAtSlot and log writes to member+2 with
     the caller's LR (watchpoint.cpp is Linux-only).
  2. Rank/details of player 2 are copied from player 1; sub_821A44A0(party, controller,
     slot) fills a member from a controller's own profile, but also writes party+5368.
  3. Joining from PC2 with 2 players (joinParty carries one player a machine).
  4. Release build + test kit for both PCs; docs for the final UX; push at stage end.
- Never propose one game copy per player: the user wants native split screen only.
- Diagnostic Windows builds needed `pthread` linked on Windows (clock_gettime).
- New test builds can reuse an installed `game\` folder (junctions to stage0-stock);
  git identity is popcan85 with the GitHub noreply email.
