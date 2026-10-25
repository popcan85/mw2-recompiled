#include "optimise.h"
#include "../runtime/install/spawn.h"
#include "disc.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace
{
#ifdef _WIN32
    constexpr const char* kMultiplayer = "mw2-mp.exe";
#else
    constexpr const char* kMultiplayer = "mw2-mp";
#endif
    constexpr const char* kResults = "benchmark.txt";
    constexpr auto kLongestRun = std::chrono::minutes(6);

    std::map<std::string, std::string> KeyValues(std::istream& in)
    {
        std::map<std::string, std::string> values;
        for (std::string line; std::getline(in, line);)
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            const size_t equals = line.find('=');
            if (equals != std::string::npos) values[line.substr(0, equals)] = line.substr(equals + 1);
        }
        return values;
    }

    std::string Rounded(double value) { return std::to_string(int(std::lround(value))); }

    // One benchmark at `choice`, its shaders compiled already.
    optimise::Measured RunOne(int players, settings::Ratio ratio, const optimise::Choice& choice, const std::string& device,
                              setup::Progress& progress)
    {
        optimise::Measured measured;
        measured.choice = choice;
        const settings::Resolution& row = settings::Resolutions(ratio)[choice.row];
        std::error_code ec;
        const fs::path results = fs::absolute(kResults, ec);
        fs::remove(results, ec);

        SDL_Environment* environment = SDL_CreateEnvironment(true);
        auto set = [&](const char* name, const std::string& value) { SDL_SetEnvironmentVariable(environment, name, value.c_str(), true); };
        set("MW2_BENCHMARK", std::to_string(players));
        set("MW2_BENCHMARK_OUT", install::Utf8(results));
        set("MW2_RENDER", std::to_string(row.width) + "x" + std::to_string(row.height));
        set("MW2_SCALE", std::to_string(row.scale));
        set("MW2_NO_MSAA", choice.msaa == 0 ? "1" : "0");
        set("MW2_MSAA", std::to_string(std::max(choice.msaa, 2)));
        set("MW2_FPS_LIMIT", "0");
        set("MW2_FULLSCREEN", "1");
        if (!device.empty()) set("MW2_SHADERS_COMPILED_FOR", device);
        const std::string program = install::Utf8(fs::absolute(kMultiplayer, ec));
        const char* const args[] = { program.c_str(), nullptr };
        SDL_Process* process = spawn::Start(args, environment);
        SDL_DestroyEnvironment(environment);
        if (!process)
        {
            measured.error = std::string(kMultiplayer) + " could not be started: " + SDL_GetError();
            return measured;
        }
        const auto from = std::chrono::steady_clock::now();
        int code = 0;
        while (!SDL_WaitProcess(process, false, &code))
        {
            if (progress.cancel || std::chrono::steady_clock::now() - from > kLongestRun)
            {
                SDL_KillProcess(process, true);
                SDL_WaitProcess(process, true, &code);
                measured.error = progress.cancel ? "stopped" : "the game took too long";
                SDL_DestroyProcess(process);
                return measured;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        SDL_DestroyProcess(process);

        std::ifstream in(results);
        const auto values = KeyValues(in);
        const auto result = values.find("result");
        if (result == values.end())
        {
            measured.error = "the game ended without a result";
            return measured;
        }
        if (result->second != "ok")
        {
            measured.error = "the benchmark " + result->second;
            return measured;
        }
        measured.fps = std::atof(values.count("fps") ? values.at("fps").c_str() : "0");
        measured.low1 = std::atof(values.count("low1") ? values.at("low1").c_str() : "0");
        measured.ok = measured.fps > 0;
        if (!measured.ok) measured.error = "the benchmark counted no frames";
        return measured;
    }
}

std::vector<optimise::Choice> optimise::Ladder(settings::Ratio ratio, int screenHeight)
{
    const auto rows = settings::Resolutions(ratio);
    // The screen's own: the tallest row no taller than it (a twentieth over is
    // its own still); the first when every one is.
    int native = 0;
    for (int i = 0; i < int(rows.size()); i++)
        if (int(rows[i].height) * rows[i].scale * 20 <= screenHeight * 21) native = i;
    std::vector<Choice> ladder;
    if (native + 1 < int(rows.size())) ladder.push_back({ native + 1, 4 });
    ladder.push_back({ native, 4 });
    ladder.push_back({ native, 2 });
    ladder.push_back({ native, 0 });
    for (int i = native - 1; i >= 0; i--) ladder.push_back({ i, i == 0 ? 0 : 2 });
    return ladder;
}

std::string optimise::Name(settings::Ratio ratio, const Choice& choice)
{
    return std::string(settings::Resolutions(ratio)[choice.row].name) + ", " +
           (choice.msaa ? std::to_string(choice.msaa) + "x antialiasing" : std::string("no antialiasing"));
}

setup::Result optimise::Find(int players, int target, settings::Ratio ratio, int screenHeight, setup::Progress& progress,
                             precompile::Progress& compile, Outcome& outcome, std::string& error)
{
    outcome = {};
    outcome.players = players;
    outcome.target = target;
    outcome.ratio = ratio;
    const std::vector<Choice> ladder = Ladder(ratio, screenHeight);
    const int lightest = int(ladder.size()) - 1;
    int halvings = 0;
    for (int span = lightest; span > 1; span = (span + 1) / 2) halvings++;
    {
        std::lock_guard lock(progress.lock);
        progress.steps = 2 + halvings;
        progress.step = 0;
    }

    std::set<int> compiled;   // the scales whose shaders are ready this time
    std::string device;
    std::map<int, Measured> measured;
    bool failed = false;
    auto run = [&](int index) -> const Measured& {
        if (const auto known = measured.find(index); known != measured.end()) return known->second;
        const Choice& choice = ladder[index];
        const int scale = settings::Resolutions(ratio)[choice.row].scale;
        if (!compiled.count(scale))
        {
            {
                std::lock_guard lock(progress.lock);
                progress.title = std::string("Compiling the shaders for ") + settings::Resolutions(ratio)[choice.row].name;
                progress.detail = "Once for each resolution: a minute or two.";
            }
            compile.shaders = 0;
            compile.compiled = 0;
            const precompile::Result result = precompile::Run("", false, compile, uint32_t(scale));
            if (!result.ok)
            {
                Measured fail;
                fail.choice = choice;
                fail.error = result.cancelled ? "stopped" : "the shaders could not be compiled: " + result.error;
                failed = true;
                return measured[index] = fail;
            }
            device = result.deviceId;
            compiled.insert(scale);
        }
        {
            std::lock_guard lock(progress.lock);
            progress.step++;
            progress.title = "Benchmark " + std::to_string(progress.step) + ": " + Name(ratio, choice);
            progress.detail = std::to_string(players) + (players == 1 ? " player" : " players") +
                              " on Terminal, about 40 seconds. The game shows full screen meanwhile.";
        }
        Measured result = RunOne(players, ratio, choice, device, progress);
        if (!result.ok) failed = true;
        outcome.runs.push_back(result);
        return measured[index] = result;
    };

    const Measured& light = run(lightest);
    if (failed) return progress.cancel ? setup::Result::Cancelled : (error = light.error, setup::Result::Failed);
    outcome.most = light.fps;
    // The target with a tenth to spare, or as near the most as can be.
    const double goal = std::min(target > 0 ? target * 1.1 : light.fps, light.fps * 0.95);
    // A run can come out slow once -- the driver warming up after a new
    // build, say -- so one that misses by a little is run again, and the
    // better of the two counts.
    std::set<int> retried;
    auto good = [&](int index) {
        const Measured& m = run(index);
        if (m.ok && m.fps < goal && m.fps >= goal * 0.92 && !retried.count(index) && !progress.cancel)
        {
            retried.insert(index);
            const Measured first = m;
            measured.erase(index);
            const Measured& again = run(index);
            if (again.ok && again.fps < first.fps) measured[index] = first;
        }
        const Measured& counted = measured[index];
        return counted.ok && counted.fps >= goal;
    };
    int chosen = lightest;
    if (good(0)) chosen = 0;
    else if (!failed)
    {
        int bad = 0, fine = lightest;
        while (fine - bad > 1 && !failed)
        {
            const int middle = (bad + fine) / 2;
            if (good(middle)) fine = middle;
            else bad = middle;
        }
        chosen = fine;
    }
    if (failed)
    {
        if (progress.cancel) return setup::Result::Cancelled;
        for (const Measured& m : outcome.runs)
            if (!m.ok) error = m.error;
        return setup::Result::Failed;
    }
    outcome.found = true;
    outcome.choice = ladder[chosen];
    outcome.fps = measured[chosen].fps;
    return setup::Result::Done;
}

std::string optimise::Summary(const Outcome& outcome)
{
    if (!outcome.found) return "";
    const std::string who = std::to_string(outcome.players) + (outcome.players == 1 ? " player" : " players");
    std::string text = who + ": " + Name(outcome.ratio, outcome.choice) + ", about " + Rounded(outcome.fps) + " frames a second";
    if (outcome.target > 0) text += " (the target is " + std::to_string(outcome.target) + ")";
    text += ".";
    const bool atMost = outcome.fps >= outcome.most * 0.95;
    if (outcome.target > 0 && outcome.fps >= outcome.target)
        text += "\n\nIt keeps the target with room to spare.";
    else if (atMost)
        text += "\n\nThis computer reaches about " + Rounded(outcome.most) + " with " + who +
                " whatever the settings: its processor sets the pace there, so lighter settings would not be faster, "
                "and these cost next to nothing.";
    else
        text += "\n\nThese are the sharpest settings that come near the target; the lightest reach about " +
                Rounded(outcome.most) + ".";
    return text;
}

std::string optimise::Encode(const Outcome& outcome)
{
    std::ostringstream out;
    out << outcome.choice.row << ',' << outcome.choice.msaa << ',' << int(std::lround(outcome.fps)) << ','
        << int(std::lround(outcome.most)) << ',' << outcome.target << ',' << int(outcome.ratio);
    return out.str();
}

bool optimise::Decode(const std::string& text, Outcome& outcome)
{
    int row = 0, msaa = 0, fps = 0, most = 0, target = 0, ratio = 0;
    if (std::sscanf(text.c_str(), "%d,%d,%d,%d,%d,%d", &row, &msaa, &fps, &most, &target, &ratio) != 6) return false;
    if (ratio < 0 || ratio >= settings::kRatios) return false;
    outcome.ratio = settings::Ratio(ratio);
    if (row < 0 || row >= int(settings::Resolutions(outcome.ratio).size())) return false;
    outcome.found = true;
    outcome.choice = { row, msaa };
    outcome.fps = fps;
    outcome.most = most;
    outcome.target = target;
    return true;
}

std::string optimise::LastRealPlay()
{
    std::ifstream in("performance.txt");
    std::string last;
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) last = line;
    if (last.empty()) return "";
    std::map<std::string, std::string> values;
    std::istringstream words(last);
    std::string date, time;
    words >> date >> time;
    for (std::string word; words >> word;)
        if (const size_t equals = word.find('='); equals != std::string::npos) values[word.substr(0, equals)] = word.substr(equals + 1);
    auto get = [&](const char* key) { return values.count(key) ? values[key] : std::string("?"); };
    const bool hosting = get("hosting") == "1";
    std::string text = "The last match, " + date + " " + time + " on " + get("map") + ": ";
    text += get("local") + " here";
    if (hosting) text += ", hosting " + get("players") + " in all";
    text += ", about " + Rounded(std::atof(get("fps").c_str())) + " frames a second (the slowest hundredth " +
            Rounded(std::atof(get("low1").c_str())) + ").";
    return text;
}
