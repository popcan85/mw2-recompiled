// mw2-setup: one program a copy of the game is made from, put beside the
// disc image. tools/make_setup.py appends to it what it carries -- the
// launcher, the campaign, the multiplayer, the settings, whatever else is
// given -- as a payload: each file as its name's length (4 bytes), its name
// (UTF-8), its size (8 bytes) and its bytes, then where the first starts (8
// bytes) and "MW2SETUP". Everything little-endian.
//
// Run, it writes the files into its own folder, beside the image: a newer
// copy of each program in place of the old, and the switches of the .env it
// carries added to the .env there that does not have them yet. It names the
// image in .env (MW2_INSTALL_DISC), puts a shortcut to the launcher on the
// desktop and starts the launcher, which installs the game from the image
// as INSTALL GAME does. Run again, it brings the programs up to date and
// leaves the installed game as it is.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr const wchar_t* kTitle = L"Modern Warfare 2 setup";
    constexpr char kMagic[8] = { 'M', 'W', '2', 'S', 'E', 'T', 'U', 'P' };
    constexpr uint64_t kGameBytes = 8ull << 30;   // the disc's files and the room the shaders take

    struct File
    {
        std::string name;
        std::string bytes;
    };

    void Say(const std::wstring& text, UINT icon = MB_ICONINFORMATION) { MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | icon); }

    std::wstring Wide(const std::string& utf8)
    {
        if (utf8.empty()) return {};
        std::wstring out(MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), int(out.size()));
        return out;
    }

    std::string Utf8(const std::wstring& wide)
    {
        if (wide.empty()) return {};
        std::string out(WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), int(out.size()), nullptr, nullptr);
        return out;
    }

    std::wstring Lower(std::wstring text)
    {
        for (wchar_t& c : text) c = wchar_t(std::towlower(c));
        return text;
    }

    fs::path Self()
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD got = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
            if (got < path.size())
            {
                path.resize(got);
                return path;
            }
            path.resize(path.size() * 2);
        }
    }

    // What make_setup.py appended to this program, or nothing.
    bool ReadPayload(const fs::path& self, std::vector<File>& files)
    {
        std::ifstream in(self, std::ios::binary);
        if (!in) return false;
        in.seekg(0, std::ios::end);
        const uint64_t end = uint64_t(in.tellg());
        if (end < 16) return false;
        in.seekg(std::streamoff(end - 16));
        uint64_t start = 0;
        char magic[8];
        in.read(reinterpret_cast<char*>(&start), 8);
        in.read(magic, 8);
        if (!in || std::memcmp(magic, kMagic, 8) != 0 || start >= end - 16) return false;
        in.seekg(std::streamoff(start));
        while (uint64_t(in.tellg()) < end - 16)
        {
            uint32_t nameLength = 0;
            uint64_t size = 0;
            File file;
            in.read(reinterpret_cast<char*>(&nameLength), 4);
            if (!in || nameLength == 0 || nameLength > 4096) return false;
            file.name.resize(nameLength);
            in.read(file.name.data(), nameLength);
            in.read(reinterpret_cast<char*>(&size), 8);
            if (!in || size > end) return false;
            file.bytes.resize(size_t(size));
            in.read(file.bytes.data(), std::streamsize(size));
            if (!in) return false;
            files.push_back(std::move(file));
        }
        return true;
    }

    // The disc image beside this program: the one named for the game if there
    // are several.
    fs::path FindDisc(const fs::path& folder)
    {
        std::vector<fs::path> images;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(folder, ec))
            if (entry.is_regular_file(ec) && Lower(entry.path().extension().wstring()) == L".iso") images.push_back(entry.path());
        std::sort(images.begin(), images.end());
        for (const fs::path& image : images)
            if (Lower(image.filename().wstring()).find(L"modern warfare 2") != std::wstring::npos) return image;
        return images.empty() ? fs::path() : images.front();
    }

    // The file written whole, or the reason it was not.
    bool Write(const fs::path& to, const std::string& bytes, std::wstring& why)
    {
        fs::path part = to;
        part += L".part";
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), std::streamsize(bytes.size()));
            if (!out)
            {
                why = L"Could not write " + part.wstring() + L" (is the disk full?).";
                return false;
            }
        }
        if (!MoveFileExW(part.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileW(part.c_str());
            why = L"Could not replace " + to.wstring() + L". Close the game and the launcher, then run setup again.";
            return false;
        }
        return true;
    }

    // NAME=value lines: the ones the folder's .env lacks are added, and the
    // disc named.
    std::string MergeSettings(const fs::path& path, const std::string& carried, const std::string& disc)
    {
        auto keyOf = [](const std::string& line) {
            const size_t equals = line.find('=');
            return equals == std::string::npos ? std::string() : line.substr(0, equals);
        };
        std::vector<std::string> lines;
        {
            std::ifstream in(path, std::ios::binary);
            for (std::string line; std::getline(in, line);)
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(line);
            }
        }
        std::map<std::string, size_t> have;
        for (size_t i = 0; i < lines.size(); i++)
            if (const std::string key = keyOf(lines[i]); !key.empty()) have[key] = i;
        std::istringstream in(carried);
        for (std::string line; std::getline(in, line);)
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string key = keyOf(line);
            if (key.empty() || have.count(key)) continue;
            have[key] = lines.size();
            lines.push_back(line);
        }
        const std::string named = "MW2_INSTALL_DISC=" + disc;
        if (const auto at = have.find("MW2_INSTALL_DISC"); at != have.end()) lines[at->second] = named;
        else lines.push_back(named);
        std::string out;
        for (const std::string& line : lines) out += line + "\n";
        return out;
    }

    // A shortcut on the desktop to the launcher, or false.
    bool Shortcut(const fs::path& launcher)
    {
        PWSTR desktop = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) return false;
        const fs::path link = fs::path(desktop) / L"MW2 Split Screen.lnk";
        CoTaskMemFree(desktop);
        bool made = false;
        IShellLinkW* shell = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&shell))))
        {
            shell->SetPath(launcher.c_str());
            shell->SetWorkingDirectory(launcher.parent_path().c_str());
            shell->SetDescription(L"Modern Warfare 2, split screen on the LAN");
            IPersistFile* file = nullptr;
            if (SUCCEEDED(shell->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))))
            {
                made = SUCCEEDED(file->Save(link.c_str(), TRUE));
                file->Release();
            }
            shell->Release();
        }
        return made;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const fs::path self = Self();
    const fs::path folder = self.parent_path();
    std::vector<File> files;
    if (!ReadPayload(self, files) || files.empty())
    {
        Say(L"This setup carries no game files: it was not packed (tools/make_setup.py).", MB_ICONERROR);
        return 1;
    }

    std::error_code ec;
    const bool installed = fs::exists(folder / L"game" / L"default_mp.xex", ec);
    const fs::path disc = FindDisc(folder);
    if (disc.empty() && !installed)
    {
        Say(L"Put the Modern Warfare 2 disc image (.iso) in this folder, beside setup, and run it again:\n\n" + folder.wstring(),
            MB_ICONWARNING);
        return 1;
    }
    if (!installed)
    {
        ULARGE_INTEGER free{};
        if (GetDiskFreeSpaceExW(folder.c_str(), &free, nullptr, nullptr) && free.QuadPart < kGameBytes)
        {
            Say(L"The game needs about 8 GB in this folder's drive, and " + std::to_wstring(free.QuadPart >> 30) +
                    L" GB are free. Make room, or move this folder to another drive.",
                MB_ICONWARNING);
            return 1;
        }
    }
    const std::wstring question =
        installed ? L"Modern Warfare 2 is installed in this folder. Bring its programs up to date?\n\n" + folder.wstring()
                  : L"Install Modern Warfare 2 (split screen on the LAN) in this folder?\n\n" + folder.wstring() +
                        L"\n\nFrom " + disc.filename().wstring() + L": about 7 GB is copied. The launcher opens and shows the progress.";
    if (MessageBoxW(nullptr, question.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;

    for (const File& file : files)
    {
        const fs::path to = folder / Wide(file.name);
        std::wstring why;
        if (file.name == ".env")
        {
            const std::string merged = MergeSettings(to, file.bytes, disc.empty() ? std::string() : Utf8(disc.filename().wstring()));
            if (!Write(to, merged, why)) return Say(why, MB_ICONERROR), 1;
            continue;
        }
        if (!Write(to, file.bytes, why)) return Say(why, MB_ICONERROR), 1;
    }

    const fs::path launcher = folder / L"mw2-launcher.exe";
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool linked = Shortcut(launcher);
    if (SUCCEEDED(com)) CoUninitialize();
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", launcher.c_str(), nullptr, folder.c_str(), SW_SHOWNORMAL)) <= 32)
    {
        Say(L"The files are in place, but the launcher did not start. Open mw2-launcher.exe in this folder.", MB_ICONWARNING);
        return 1;
    }
    if (!linked) Say(L"No shortcut could be put on the desktop. Open mw2-launcher.exe in this folder to play.", MB_ICONWARNING);
    return 0;
}
