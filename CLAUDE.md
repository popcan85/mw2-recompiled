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
  (`install.bat` installs from the ISO once, `launch.bat` runs it). Awaiting user's test.
