#pragma once
// OPTIMISE: the sharpest settings that keep the frame rate a player wants,
// found by running the multiplayer's benchmark (runtime/benchmark.h) at one
// setting after another.
//
// The settings are the graphics screen's own: a RESOLUTION of the shape kept
// (its frame and scale) and ANTIALIASING. They are tried from the sharpest --
// one resolution above the screen's own, drawn larger and made smaller -- to
// the lightest. The lightest goes first: what it reaches is the most this
// computer manages with that many players, whatever is drawn, since the
// processor can hold the frame rate down before the graphics card does. The
// sharpest goes next, and is taken if it keeps the target with a tenth to
// spare, or comes within a twentieth of that most; else the ladder between is
// halved until the sharpest that does is found. A run that misses by less
// than a twelfth is run once more, and the better of the two counts. A
// resolution's shaders are compiled once, the first time it is tried (each
// scale keeps its own).
//
// What it found for each number of players is kept in .env
// (MW2_OPTIMISED_<players>), for PLAYERS ON THIS PC to put back.
#include "precompile.h"
#include "setup.h"
#include "settings.h"

#include <string>
#include <vector>

namespace optimise
{
    struct Choice
    {
        int row = 0;     // of settings::Resolutions(ratio)
        int msaa = 2;    // 0, 2, 4
    };

    struct Measured
    {
        Choice choice;
        bool ok = false;
        double fps = 0, low1 = 0;
        std::string error;
    };

    struct Outcome
    {
        bool found = false;
        Choice choice;
        double fps = 0;          // what the choice reached
        double most = 0;         // what the lightest reached: the computer's most
        int players = 0, target = 0;
        settings::Ratio ratio = settings::Ratio::Wide;
        std::vector<Measured> runs;
    };

    // The settings, sharpest first, for a screen `screenHeight` pixels tall.
    std::vector<Choice> Ladder(settings::Ratio ratio, int screenHeight);

    // "2560x1440, 4x antialiasing".
    std::string Name(settings::Ratio ratio, const Choice& choice);

    // Runs the benchmarks; blocks. `compile` shows a resolution's shaders being
    // compiled. Done with `outcome` filled, Cancelled, or Failed with `error`.
    setup::Result Find(int players, int target, settings::Ratio ratio, int screenHeight, setup::Progress& progress,
                       precompile::Progress& compile, Outcome& outcome, std::string& error);

    // What Find found, said for the player, and kept for PLAYERS ON THIS PC.
    std::string Summary(const Outcome& outcome);
    std::string Encode(const Outcome& outcome);
    bool Decode(const std::string& text, Outcome& outcome);

    // The last line of performance.txt, the game's record of real play
    // (runtime/play_record.h), said for the player; empty when there is none.
    std::string LastRealPlay();
}
