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
  Session 2026-10-24 (stage 2d, UNCOMMITTED until the user's two-PC test):
  - Name flip found: Party_RefreshLocalMembers (821A30D8) writes the given controller's
    gamertag into EVERY member signed in here; hook puts each member's own name back.
  - Local players now added with PartyHost_AddLocalPlayer (821A44A0, own profile: rank
    etc.), party+5368 saved/restored.
  - PC2 with 2 players: party commands table 0x82020848 (joinParty, memberJoin, rank...).
    PC2 resends the title's memberJoin (Party_SendMemberJoin 82198798, token/address
    recorded from the first player's) for controller 2 once the party lists player 1.
    Host's memberJoin handler adds a XUID differing from the member at that address;
    hook marks it g_beside (FindMemberByAddr skips it unless alone; timeout +52 copied
    from his machine's first; CopyBeside puts him in the other hosted party).
    joinParty removes all members at an address (so a rejoin clears both).
  - Kits: `builds\stage2d-two-pc\` (PC1) and `builds\stage2d-pc2-files\` (copy the two
    files into PC2's pc2-kit; PC2 was offline). Both diagnostic builds.
  - User confirmed on PC1: names distinct in lobby and in game. FOCUS: polish the one-PC
    experience before any two-PC test (user's call).
  - Join prompts: hook CL_DrawScreen2D (82179370, last 2D draw of the frame), draw with
    R_AddCmdDrawText/normalfont at lower right: "Controller N: press START to join" for
    connected (pad read <1 s ago) unjoined controllers, "<name>: hold BACK to leave".
    Built into `builds\stage2d-two-pc\`; NOT yet seen by the user (placement/size guesses).
  - User confirmed the join prompts work.
  - MW2_UNLOCK_ALL=1 (runtime/unlock_all.cpp, called each frame from input.cpp): each
    controller's in-memory stats (0x830D7DF4 + c*25136, two copies of 8196: CRC + 8188
    buffer) get the launcher's RANK 70 max + UNLOCK EVERYTHING when version/format match
    and experience != max; CRC redone with sub_82379A40. In the PC1 kit's launch.bat.
    Saves backed up to `builds\saves-backup-2026-10-24\`. NOT yet tested by the user.
  - User test: unlock works in memory (rank 70) but "Not uploading stats: no
    statWriteNeeded" -> now set stats+8193 after unlocking. CAC greyed in private lobby:
    isItemUnlocked (822D1440) says locked when cl_multi_gamepads_enabled && conn<5 ->
    hooked, multi cleared for the call. Split Screen Setup opened when START rejoined a
    controller still mapped (makeLocalClientActive on a mapped controller = sub_821AC0E0,
    "closemenu menu_splitscreensignin; openmenu menu_gamesetup_splitscreen") -> Join skips
    it when mapped; leaving Xbox LIVE now runs Leave + multi=0. Name refresh ran every
    frame (our post-fix churned) -> other local members set to status 2 during the call.
    Controller icons (xenon_controller_top + number) left of local rows: rows guessed
    from the screenshot (x 807, first row y 111, step 30 at 1280x720; party1 slot order).
    All in `builds\stage2d-two-pc\`, NOT yet tested.
  - User: all good but P2 rank 1 after restart. Cause: member made 4 ms before his stats
    download (stats were saved at 70). KeepDetails refreshes member +124/+148/+152/+156
    from the AddLocalPlayer readers each frame. Leave now signs out (signin::SignOut).
    Sign-in screen: New profile opens an on-screen keyboard (suggested name), X renames a
    listed profile (RenameLocked rewrites profiles.txt). Player 1 rename: not in game yet.
    In `builds\stage2d-two-pc\`, NOT yet tested.
  - User CONFIRMED one-PC: 4 players in private match (Xbox One, Pro, PS4, DualSense),
    names/ranks/unlock/sign-in keyboard all fine. Committed 06689c4 (not pushed).
    Parked: (a) Pro Controller could not join when it was not controller 0 (START reached
    the menus) -- need a log of that run; (b) split-screen performance (~130 fps, GPU 53%):
    the title lowers resolution/fidelity in split screen; look later.
  - PC2 kit: `\\192.168.0.144\mw2\builds\pc2-kit\launch-stage2d.bat` + mw2-mp-stage2d.exe
    (SPLIT_LIVE, UNLOCK_ALL, diagnostic); PC2 saves backed up to pc2-kit\saves-backup-2026-10-24.
  - Two-PC test 1: PC2 joined (fine while PC1 had 1 player), but with PC1's extra
    "Todd" (slot 0, PC1's address) PC2 migrated: "Migrating the party over so clientNum 0
    (Todd) is the new host" -> "Unable to migrate hosts". party+5200 is the HOST's slot
    (sent in partystate), not "me" (renamed T_PARTY_HOST). FindMemberByAddr on every
    machine now returns the host for a member at the host's address. Rebuilt into both
    kits (PC1 stage2d-two-pc, PC2 launch-stage2d.bat). PC2's lobby also drew a
    split-screen player card ("Todd") over the list -- recheck after this fix.
  - Two-PC test 2: step 3 OK (no migration, cards fine). PC2's player 2 failed: PC1
    "Challenge key did not match" (memberJoin claims a status-2 slot reserved by joinParty
    with the challenge at member+44; PC2's joinParty asked room for 1) -> PC1 told PC2 to
    rejoin -> joinParty cleared Eunbin. Also Party_SendMemberJoin marks PC2's party as
    joining (+5288..5311) and PC2's 0.05 s out-of-lobby made Leave sign suuuuh out while
    primary=1 -> "active profile signed out" -> main menu. Fixes: host reserves a slot
    (PartyHost_ReserveSlot 821A05E0) under the extra's challenge before the handler;
    SendMemberJoin restores +5288..5311; out-of-lobby leave needs 2 s; Leave resets
    primary/menu controller to 0 before signing out. Rebuilt into both kits.
  - Test 3: reserve+claim worked but the claim (821A6258) looks up a member at the
    sender's address and, finding Eunbin, updates him instead of AddPlayerAtSlot; PC2
    resent every 3 s (join sound loop). Fix: g_adding makes FindMemberByAddr return -1
    for that address during the handler until the new XUID is a member.
  - Test 4: LOBBY WORKS across PCs (PC2's 2nd player listed on both). Remaining: rank 1
    for him (memberJoin sent before his stats read -> now waits for stats+8192 seen a
    frame earlier); glyphs on PC2's loading screen (prompts now need LocalClientCount 0);
    match start -> PC2 "EXE_BAD_CHALLENGE": challengeResponse carries no owner and PC2's
    client 0 took client 1's challenge; SV_GetChallenge keys by address + key string.
    Fix: server appends the getchallenge key to challengeResponse (hooks SV_GetChallenge
    822BA9B8 + NET_OutOfBandPrint 8228EE28); client_packet_sender asks
    splitlive::ForAnotherClient to pass it on. connectResponse has the same ambiguity
    (not handled yet). Leaving on PC2 only local (known).
  - Test 5: suuuuh joins at rank 70, match starts with no error, but only PC2's player 1
    gets in: CL_ConnectFromParty re-runs CL_InitLocalClients per client and the re-init
    disconnects all (the "already set up" check fails under the dvar copy), so connecting
    client 1 dropped half-connected client 0; PC2 then reset and reconnected client 0
    alone. Fix: hook skips the setup when not hosting, 4 clients exist and one is
    connecting (state > 0). Test 6: same result -- CL_ConnectFromParty (82189C30) itself
    disconnects all (sub_82189E40) then connects every active client; the title calls it
    per client, so the 2nd call undid the 1st. Init skip removed; now ConnectFromParty is
    passed over for a client > 0 already connecting. Test 7: no change -- the skip never
    fired: each call disconnects all active clients (sub_821839F0), sets the server
    address in every active client's connection, but state 3 only for the client called;
    the party "go" (sub_8219B1E8) calls it for each active client, so only the last
    survives. Now after the first call every other active client is set to state 3 (not
    when sv_running), so later calls are passed over. PC1 (host) never calls it.
  - Test 8: both PC2 clients connect; "Error synchronizing game settings": the server's
    stats handler finds the client by SV_FindClientByAddr (822C1918), which compares IP
    only (8228E4B0); statResponse unowned ("Dup statResponse"). General fix: each extra
    local client is its own port (socket port + 100*client): NET_SendPacket (8228EBF0)
    hook records the sock (= local client), kernel/net.cpp sendto sends from that port,
    recvfrom also polls port+100*c and calls splitlive::ReceivedFor(c);
    client_packet_sender's CL_PacketEvent refuses packets for another client
    (PacketForAnother); server's FindClientByAddr hook prefers the exact port.
    Rebuilt into both kits. (The challengeResponse tagging stays.)
    Tabled by user: PC1's joining player 2 makes no join sound (leave sound plays; we use
    AddLocalPlayer now; sound source not found yet); a PC that quits stays listed ~12 s
    (party timeout) -- faster would be nicer; split-screen performance.
  OPEN: user's two-PC test (again); leaving on PC2 is local only (host keeps him until PC2
  leaves); then release build + kits, final docs, commit, push.
- Never propose one game copy per player: the user wants native split screen only.
- Diagnostic Windows builds needed `pthread` linked on Windows (clock_gettime).
- New test builds can reuse an installed `game\` folder (junctions to stage0-stock);
  git identity is popcan85 with the GitHub noreply email.
