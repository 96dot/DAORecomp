#include "stdafx.h"
#include "mods/mod_install.h"

#include <fstream>
#include <regex>
#include <unordered_set>

#include "files.h"
#include "handle.h"
#include "mods/mod_files.h"
#include "text.h"

#include "xenia/base/cvar.h" // toml++

namespace dao::mods
{
    namespace fs = std::filesystem;
    using Line = InstallResult::Line;

    namespace
    {
        // The work folder in mods/ (hidden from the mod list): a folder per launcher
        // adding mods, locked while it does, with a folder per drop.
        constexpr std::string_view kWorkFolder = ".adding";
        static_assert(kWorkFolder.front() == kHiddenPrefix);
        // Folders that say where their contents go: name -> place in the mod.
        constexpr std::pair<std::string_view, std::string_view> kAnchors[] = {
            { "packages", "packages" },
            { "core", "packages/core" },
            { "override", "packages/core/override" },
        };
        // How deep to look for one of them.
        constexpr int kAnchorDepth = 3;
        // Folders wrapping a mod whose name isn't the mod's.
        constexpr std::string_view kGenericNames[] = { "data", "files", "main", "main files", "mod", "mods",
            "content", "contents", "install" };
        // Folder names of parts the player may have to choose between.
        constexpr std::string_view kOptionalWords[] = { "optional", "alternat", "variant", "choose" };
        // The name of a mod that has no better one (loose files).
        constexpr std::string_view kNewMod = "New mod";
        constexpr size_t kMaxNameBytes = 100;
        // A bigger Manifest.xml isn't one; longer fields are cut.
        constexpr uintmax_t kMaxManifestBytes = 1 << 20;
        constexpr size_t kMaxFieldBytes = 4096;
        // Unpacking or copying stops when the drive gets this full; unpacking also
        // after this long.
        constexpr uintmax_t kMinFreeBytes = 1ull << 30;
        constexpr auto kMaxExtractTime = std::chrono::minutes(10);
        constexpr DWORD kExtractPollMs = 250;

        // The archives mods come in that tar.exe reads (.dazip: the PC version's
        // own, a zip).
        constexpr std::string_view kArchiveExtensions[] = { ".zip", ".7z", ".rar", ".dazip" };

        bool HasExtension(const fs::path& path, std::span<const std::string_view> extensions)
        {
            return std::ranges::find(extensions, Lower(Utf8(path.extension()))) != extensions.end();
        }

        // A real folder or file: not a link (symbolic, or a junction) to one.
        bool IsFolder(const fs::directory_entry& entry)
        {
            std::error_code ec;
            return entry.symlink_status(ec).type() == fs::file_type::directory;
        }

        bool IsFile(const fs::directory_entry& entry)
        {
            std::error_code ec;
            return entry.symlink_status(ec).type() == fs::file_type::regular;
        }

        // `text` cut to at most `bytes`, not in the middle of a character.
        std::string Truncate(std::string text, size_t bytes)
        {
            if (text.size() > bytes)
            {
                while (bytes > 0 && (uint8_t(text[bytes]) & 0xC0) == 0x80)
                    bytes--;
                text.resize(bytes);
            }
            return text;
        }

        fs::path TarPath()
        {
            wchar_t system[MAX_PATH];
            const UINT length = GetSystemDirectoryW(system, MAX_PATH);
            if (length == 0 || length >= MAX_PATH)
                return {};
            return fs::path(system) / L"tar.exe";
        }

        // Unpacks an archive with Windows' tar.exe (libarchive: zip, 7z, rar...),
        // which by default leaves out entries with absolute paths or "..". It's
        // stopped when `stop` is requested, after kMaxExtractTime or when the
        // drive gets nearly full.
        bool Extract(const fs::path& archive, const fs::path& into, std::stop_token stop, std::string& error)
        {
            const fs::path tar = TarPath();
            std::wstring command = L"\"" + tar.wstring() + L"\" -x -f \"" + archive.wstring() + L"\" -C \"" +
                into.wstring() + L"\"";
            // In a job, so it ends with the launcher.
            const Handle job(CreateJobObjectW(nullptr, nullptr));
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            STARTUPINFOW startup = { .cb = sizeof(startup) };
            PROCESS_INFORMATION info = {};
            // Detached (no console), so nothing can make it wait for input such as an
            // encrypted archive's password; suspended until it's in the job.
            if (!job ||
                !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                !CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, FALSE,
                    DETACHED_PROCESS | CREATE_SUSPENDED, nullptr, into.c_str(), &startup, &info))
            {
                error = fmt::format("can't run tar.exe (error {})", GetLastError());
                return false;
            }
            const Handle process(info.hProcess);
            const Handle thread(info.hThread);
            if (!AssignProcessToJobObject(job.get(), process.get()))
            {
                error = fmt::format("can't run tar.exe (error {})", GetLastError());
                TerminateProcess(process.get(), 1);
                return false;
            }
            ResumeThread(thread.get());

            const auto start = std::chrono::steady_clock::now();
            while (WaitForSingleObject(process.get(), kExtractPollMs) == WAIT_TIMEOUT)
            {
                std::error_code ec;
                const char* stopped = stop.stop_requested() ? "stopped"
                    : std::chrono::steady_clock::now() - start > kMaxExtractTime ? "it took too long to unpack"
                    : fs::space(into, ec).available < kMinFreeBytes && !ec ? "the drive is nearly full"
                    : nullptr;
                if (stopped)
                {
                    TerminateJobObject(job.get(), 1);
                    WaitForSingleObject(process.get(), INFINITE);
                    error = stopped;
                    return false;
                }
            }
            DWORD code = 1;
            GetExitCodeProcess(process.get(), &code);
            if (code != 0)
            {
                // (Windows' tar.exe also can't read names outside the ANSI code page.)
                error = "tar.exe can't unpack it (it may be damaged or encrypted, or have names in another "
                        "alphabet): extract it and drop the folder";
                return false;
            }
            return true;
        }

        // Copies a dropped folder or file into `to`; inside a folder, links are left
        // out (only what's really in it).
        void CopyInto(const fs::path& from, const fs::path& to, std::stop_token stop, std::error_code& ec)
        {
            const fs::file_type type = fs::symlink_status(from, ec).type();
            if (type == fs::file_type::regular)
            {
                const uintmax_t size = fs::file_size(from, ec);
                const uintmax_t free = ec ? 0 : fs::space(to.parent_path(), ec).available;
                if (!ec && free < kMinFreeBytes + size)
                    ec = std::make_error_code(std::errc::no_space_on_device);
                if (!ec)
                    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
            }
            else if (type == fs::file_type::directory)
            {
                fs::create_directories(to, ec);
                for (auto it = fs::directory_iterator(from, ec); !ec && it != fs::directory_iterator();
                     it.increment(ec))
                {
                    if (stop.stop_requested())
                        return;
                    CopyInto(it->path(), to / it->path().filename(), stop, ec);
                    if (ec)
                        return;
                }
            }
        }

        // Whether one of the two is (in) the other.
        bool Overlaps(const fs::path& a, const fs::path& b)
        {
            const auto within = [](fs::path inner, const fs::path& outer) {
                for (std::error_code ec;; inner = inner.parent_path())
                {
                    if (fs::equivalent(inner, outer, ec))
                        return true;
                    if (inner == inner.parent_path())
                        return false;
                }
            };
            std::error_code ec;
            const fs::path absoluteA = fs::absolute(a, ec), absoluteB = fs::absolute(b, ec);
            return within(absoluteA, absoluteB) || within(absoluteB, absoluteA);
        }

        // Why the unpacked or copied files can't be used, if they can't: links
        // (symbolic, junctions, hard links) could reach files outside the mod.
        std::optional<std::string> FindLink(const fs::path& dir)
        {
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
                 it.increment(ec))
            {
                const fs::file_type type = it->symlink_status(ec).type();
                if (ec)
                    break;
                if (type == fs::file_type::directory)
                    continue;
                if (type == fs::file_type::regular)
                {
                    const uintmax_t links = fs::hard_link_count(it->path(), ec);
                    if (ec)
                        break;
                    if (links == 1)
                        continue;
                }
                return fmt::format("it has a link ({}), which could reach files outside the mod",
                        WideToUtf8(it->path().lexically_relative(dir).generic_wstring()));
            }
            if (ec)
                return fmt::format("can't read its files ({})", ec.message());
            return std::nullopt;
        }

        // Files that couldn't be moved into place, and why (the last error).
        struct Moves
        {
            size_t failed = 0;
            std::error_code error;
        };

        void MoveOne(const fs::path& from, const fs::path& to, Moves& moves)
        {
            std::error_code ec;
            fs::remove(to, ec);
            fs::rename(from, to, ec);
            if (ec)
            {
                moves.failed++;
                moves.error = ec;
            }
        }

        // Moves everything in `from` into `to`, merging folders.
        void MoveInto(const fs::path& from, const fs::path& to, Moves& moves)
        {
            std::error_code ec;
            fs::create_directories(to, ec);
            for (const auto& entry : fs::directory_iterator(from, ec))
            {
                if (IsFolder(entry))
                    MoveInto(entry.path(), to / entry.path().filename(), moves);
                else if (IsFile(entry))
                    MoveOne(entry.path(), to / entry.path().filename(), moves);
            }
            // (A folder that can't be listed is left behind, as a file that can't be moved is.)
            if (ec)
            {
                moves.failed++;
                moves.error = ec;
            }
        }

        // Removes the empty folders under `dir` (left by moving files out).
        void RemoveEmptyFolders(const fs::path& dir)
        {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(dir, ec))
            {
                if (IsFolder(entry))
                {
                    RemoveEmptyFolders(entry.path());
                    fs::remove(entry.path(), ec); // Only if empty.
                }
            }
        }

        // A mod's description from a DAModder Manifest.xml (PC mods have them).
        struct Manifest
        {
            std::string type, name, version, author, description;
        };

        // XML text with its entities decoded, at most kMaxFieldBytes of it.
        std::string XmlText(std::string_view text)
        {
            static constexpr std::pair<std::string_view, char> kEntities[] = { { "&lt;", '<' }, { "&gt;", '>' },
                { "&quot;", '"' }, { "&apos;", '\'' }, { "&amp;", '&' } };
            std::string out;
            for (size_t i = 0; i < text.size() && out.size() < kMaxFieldBytes;)
            {
                const auto entity = std::ranges::find_if(kEntities, [&](const auto& e) {
                    return text.substr(i).starts_with(e.first);
                });
                if (text[i] == '&' && entity != std::end(kEntities))
                {
                    out += entity->second;
                    i += entity->first.size();
                }
                else
                    out += text[i++];
            }
            return Truncate(std::move(out), kMaxFieldBytes);
        }

        // The value of `attribute` in the first `tag` (as "<Name") that has it.
        std::string Attribute(std::string_view xml, std::string_view tag, std::string_view attribute)
        {
            for (size_t start = xml.find(tag); start != std::string_view::npos;)
            {
                const size_t close = xml.find('>', start);
                const std::string_view element = xml.substr(start, close - start);
                if (const std::string_view value = XmlAttribute(element, attribute); !value.empty())
                    return XmlText(value);
                if (close == std::string_view::npos)
                    break;
                start = xml.find(tag, close);
            }
            return {};
        }

        // The text of the first <name> element.
        std::string ElementText(std::string_view xml, std::string_view name)
        {
            const std::string open = fmt::format("<{}>", name);
            const size_t start = xml.find(open);
            if (start == std::string_view::npos)
                return {};
            const size_t text = start + open.size();
            return XmlText(xml.substr(text, xml.find('<', text) - text));
        }

        // A manifest's bytes as UTF-8: they may be UTF-16 (with its BOM) or, when not
        // UTF-8, in the Western Windows code page.
        std::string ManifestText(std::string bytes)
        {
            if (bytes.starts_with("\xFF\xFE"))
            {
                std::wstring wide((bytes.size() - 2) / 2, L'\0');
                memcpy(wide.data(), bytes.data() + 2, wide.size() * sizeof(wchar_t));
                return WideToUtf8(wide);
            }
            if (bytes.starts_with("\xEF\xBB\xBF"))
                bytes.erase(0, 3);
            return IsUtf8(bytes) ? bytes : WideToUtf8(ToWide(bytes, 1252));
        }

        std::optional<Manifest> ReadManifest(const fs::path& path)
        {
            std::error_code ec;
            if (fs::file_size(path, ec) > kMaxManifestBytes || ec)
                return std::nullopt;
            std::ifstream file(path, std::ios::binary);
            const std::string xml =
                ManifestText(std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()));
            if (xml.find("<Manifest") == std::string::npos)
                return std::nullopt;
            Manifest manifest;
            manifest.type = Attribute(xml, "<Manifest", "Type");
            // A package's title, else its add-in's name.
            manifest.name = Attribute(xml, "<Title", "DefaultText");
            if (manifest.name.empty())
                manifest.name = Attribute(xml, "<AddInItem", "Name");
            manifest.version = ElementText(xml, "Version");
            manifest.author = Attribute(xml, "<Publisher", "DefaultText");
            manifest.description = Attribute(xml, "<Description", "DefaultText");
            return manifest;
        }

        // A folder name from what was dropped: the ending of a download's name from
        // Nexus ("-<id>-<version>-<time>") removed, characters Windows doesn't allow
        // left out, short enough.
        std::string ModName(std::string name)
        {
            // Nexus's names: "<name>-<id>-<version>-<time>", or older ones "<name>-<id>"
            // and "<name>-<id>-<version>" (an ID of three digits or more, so "Armor-2"
            // stays).
            static const std::regex kNexus(R"(^(.+?)-\d+(-[0-9a-zA-Z]+)*-\d{10,}$)");
            static const std::regex kOldNexus(R"(^(.+?)-\d{3,}(-\d+)*$)");
            std::smatch match;
            // (A file's name is short: the regex isn't run on long text.)
            if (name.size() <= MAX_PATH &&
                (std::regex_match(name, match, kNexus) || std::regex_match(name, match, kOldNexus)))
                name = match[1].str();
            std::string out;
            for (const char c : name)
            {
                if (!IsBadNameChar(c))
                    out += c;
            }
            out = Truncate(std::move(out), kMaxNameBytes);
            while (!out.empty() && (out.back() == '.' || out.back() == ' '))
                out.pop_back();
            while (!out.empty() && (out.front() == kHiddenPrefix || out.front() == ' '))
                out.erase(out.begin());
            if (out.empty())
                return std::string(kNewMod);
            return IsDeviceName(out) ? out + " mod" : out;
        }

        bool IsGenericName(const fs::path& folder)
        {
            return std::ranges::find(kGenericNames, Lower(Utf8(folder.filename()))) != std::end(kGenericNames);
        }

        fs::path FreeFolder(const fs::path& modsRoot, const std::string& name)
        {
            fs::path folder = modsRoot / FromUtf8(name);
            std::error_code ec;
            for (int n = 2; fs::exists(folder, ec); n++)
                folder = modsRoot / FromUtf8(fmt::format("{} ({})", name, n));
            return folder;
        }

        // The place in the mod of a folder that says where its contents go.
        std::optional<std::string_view> AnchorPlace(const fs::path& folder)
        {
            const std::string name = Lower(Utf8(folder.filename()));
            for (const auto& [anchor, place] : kAnchors)
            {
                if (name == anchor)
                    return place;
            }
            return std::nullopt;
        }

        // The anchor folders nearest the top: all those at the shallowest depth that
        // has any, with their places.
        std::vector<std::pair<fs::path, std::string_view>> FindAnchors(const fs::path& raw)
        {
            std::vector<fs::path> level = { raw };
            for (int depth = 0; depth < kAnchorDepth && !level.empty(); depth++)
            {
                std::vector<std::pair<fs::path, std::string_view>> found;
                std::vector<fs::path> next;
                for (const fs::path& dir : level)
                {
                    std::error_code ec;
                    for (const auto& entry : fs::directory_iterator(dir, ec))
                    {
                        if (!IsFolder(entry))
                            continue;
                        if (const auto place = AnchorPlace(entry.path()))
                            found.push_back({ entry.path(), *place });
                        else
                            next.push_back(entry.path());
                    }
                }
                if (!found.empty())
                    return found;
                level = std::move(next);
            }
            return {};
        }

        // A folder of optional or alternative parts, if the mod has one near the top
        // (they're all added, so the player may have to choose).
        std::optional<std::string> FindOptionalFolder(const fs::path& dir, int depth = 0)
        {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(dir, ec))
            {
                if (!IsFolder(entry))
                    continue;
                const std::string name = Utf8(entry.path().filename());
                const std::string lower = Lower(name);
                if (std::ranges::any_of(kOptionalWords,
                        [&](std::string_view word) { return lower.find(word) != std::string::npos; }))
                    return name;
                if (depth + 1 < kAnchorDepth)
                {
                    if (auto found = FindOptionalFolder(entry.path(), depth + 1))
                        return found;
                }
            }
            return std::nullopt;
        }

        // Sorts the files of one drop (in `raw`) into a mod folder `out`; `name` is
        // the mod's name so far, maybe bettered by a wrapping folder or a manifest.
        Moves Organize(fs::path raw, const fs::path& out, std::string& name, std::optional<Manifest>& manifest,
            std::vector<Line>& notes)
        {
            Moves moves;
            std::error_code ec;
            // Unwrap single folders around everything (not one saying where it goes).
            for (;;)
            {
                std::vector<fs::directory_entry> entries;
                for (const auto& entry : fs::directory_iterator(raw, ec))
                    entries.push_back(entry);
                if (entries.size() != 1 || !IsFolder(entries[0]) || AnchorPlace(entries[0].path()))
                    break;
                raw = entries[0].path();
                if (!IsGenericName(raw))
                    name = ModName(Utf8(raw.filename()));
            }
            if (const auto folder = FindOptionalFolder(raw))
                notes.push_back({ fmt::format("It has optional parts (\"{}\"), all added: its readme may say which "
                                              "to keep.",
                                      *folder),
                    true });

            fs::create_directories(out, ec);
            // At the top: a manifest (read), mod.toml and documents stay on top.
            for (const auto& entry : fs::directory_iterator(raw, ec))
            {
                if (!IsFile(entry))
                    continue;
                const std::string file = Lower(Utf8(entry.path().filename()));
                if (file == "manifest.xml")
                    manifest = ReadManifest(entry.path());
                if (file == "manifest.xml" || file == "mod.toml" || IsDocument(entry.path()))
                    MoveOne(entry.path(), out / entry.path().filename(), moves);
            }
            if (manifest && !manifest->name.empty())
                name = ModName(manifest->name);

            for (const auto& [folder, place] : FindAnchors(raw))
            {
                MoveInto(folder, out / fs::path(place), moves);
                RemoveTree(folder);
            }
            // The rest: into override/, in a folder of the mod's name unless a DAModder
            // manifest says the mod is made for override/ itself.
            const fs::path overrideFolder = out / "packages" / "core" / "override";
            const bool coreOverride = manifest && manifest->type == "CoreOverrideAddIn";
            MoveInto(raw, coreOverride ? overrideFolder : overrideFolder / FromUtf8(name), moves);
            RemoveEmptyFolders(out);
            return moves;
        }

        // The game's files in a mod (under packages/), and how many in override/
        // have the same name as another there: the game finds those by name, so it
        // uses just one of each.
        struct Contents
        {
            size_t files = 0;
            size_t sameName = 0;
        };

        Contents Look(const fs::path& mod)
        {
            Contents contents;
            const fs::path packages = mod / "packages";
            std::unordered_set<std::string> names;
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(packages, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (std::error_code fileEc; !it->is_regular_file(fileEc))
                    continue;
                contents.files++;
                // (Its folders keep the case they were dropped with.)
                if (IsUnder(it->path().lexically_relative(packages).parent_path(), "override") &&
                    !names.insert(Lower(Utf8(it->path().filename()))).second)
                    contents.sameName++;
            }
            return contents;
        }

        // Describes the mod in mod.toml from its manifest (the launcher shows it).
        void WriteModToml(const fs::path& folder, const Manifest& manifest, const std::string& name)
        {
            std::error_code ec;
            if (fs::exists(folder / "mod.toml", ec))
                return;
            toml::table table;
            table.insert("name", manifest.name.empty() ? name : manifest.name);
            if (!manifest.version.empty())
                table.insert("version", manifest.version);
            if (!manifest.author.empty())
                table.insert("author", manifest.author);
            if (!manifest.description.empty())
                table.insert("description", manifest.description);
            std::ofstream file(folder / "mod.toml");
            file << table << "\n";
            file.close();
            if (!file)
                LOGW("Mods: can't write the mod.toml of {}", name);
        }

        // One drop (an archive, a folder or loose files) as a new mod, made in
        // `work` and then moved into mods/.
        std::optional<ModInfo> AddOne(const fs::path& modsRoot, const fs::path& work,
            const std::vector<fs::path>& sources, std::string name, std::stop_token stop, const GameFiles* game,
            std::vector<Line>& notes, std::string& error)
        {
            std::error_code ec;
            const fs::path raw = work / "raw";
            const fs::path out = work / "mod";
            fs::create_directories(raw, ec);
            if (std::ranges::any_of(sources, [&](const fs::path& source) { return Overlaps(source, modsRoot); }))
            {
                error = "it's the mods folder, in it or holds it";
                return std::nullopt;
            }
            if (sources.size() == 1 && HasExtension(sources[0], kArchiveExtensions) &&
                fs::is_regular_file(sources[0], ec))
            {
                if (!CanOpenArchives())
                {
                    error = "archives can't be opened here: extract it and drop the folder";
                    return std::nullopt;
                }
                if (!Extract(sources[0], raw, stop, error))
                    return std::nullopt;
            }
            else
            {
                for (const fs::path& source : sources)
                {
                    // What a dropped link leads to; links inside it are left out.
                    const fs::path real = fs::canonical(source, ec);
                    if (!ec)
                        CopyInto(real, raw / source.filename(), stop, ec);
                    if (ec)
                    {
                        error = fmt::format("can't copy {} ({})", Utf8(source.filename()), ec.message());
                        return std::nullopt;
                    }
                }
            }
            if (stop.stop_requested())
            {
                error = "stopped";
                return std::nullopt;
            }
            if (const auto link = FindLink(raw))
            {
                error = *link;
                return std::nullopt;
            }

            std::optional<Manifest> manifest;
            const Moves moves = Organize(raw, out, name, manifest, notes);
            if (moves.failed > 0)
            {
                error = fmt::format("{} of its files can't be put in place ({})", moves.failed, moves.error.message());
                return std::nullopt;
            }
            const Contents contents = Look(out);
            if (contents.files == 0)
            {
                error = "it has no files for the game";
                return std::nullopt;
            }
            if (contents.sameName > 0)
                notes.push_back({ fmt::format("{} of its files {} the same name as another, and the game uses one "
                                              "of each: it may have parts to choose between (see its readme).",
                                      contents.sameName, contents.sameName == 1 ? "has" : "have"),
                    true });
            if (manifest)
                WriteModToml(out, *manifest, name);
            const fs::path folder = FreeFolder(modsRoot, name);
            fs::rename(out, folder, ec);
            if (ec)
            {
                error = fmt::format("can't make its folder in mods ({})", ec.message());
                return std::nullopt;
            }
            return Describe(folder, game);
        }
    }

    bool UnpackArchive(const fs::path& archive, const fs::path& into, std::stop_token stop, std::string& error)
    {
        if (!CanOpenArchives())
        {
            error = "archives can't be opened here";
            return false;
        }
        if (!Extract(archive, into, stop, error))
            return false;
        if (const auto link = FindLink(into))
        {
            error = *link;
            return false;
        }
        return true;
    }

    bool CanOpenArchives()
    {
        static const bool available = [] {
            std::error_code ec;
            const fs::path tar = TarPath();
            return !tar.empty() && fs::exists(tar, ec);
        }();
        return available;
    }

    void DescribeFromManifest(const fs::path& modFolder, const fs::path& manifestXml)
    {
        if (const auto manifest = ReadManifest(manifestXml))
            WriteModToml(modFolder, *manifest, Utf8(modFolder.filename()));
    }

    void CleanUpInstalls(const fs::path& modsRoot)
    {
        RemoveUnlockedFolders(modsRoot / kWorkFolder);
    }

    InstallResult Install(const fs::path& modsRoot, const std::vector<fs::path>& dropped, std::stop_token stop,
        const GameFiles* game)
    {
        // Archives and folders are a mod each; loose files are one together.
        std::vector<std::pair<std::vector<fs::path>, std::string>> drops;
        std::vector<fs::path> loose;
        std::error_code ec;
        for (const fs::path& path : dropped)
        {
            if (fs::is_directory(path, ec))
                drops.push_back({ { path }, ModName(Utf8(path.filename())) });
            else if (HasExtension(path, kArchiveExtensions))
                drops.push_back({ { path }, ModName(Utf8(path.stem())) });
            else if (fs::is_regular_file(path, ec))
                loose.push_back(path);
        }
        if (!loose.empty())
            drops.push_back(
                { loose, loose.size() == 1 ? ModName(Utf8(loose[0].stem())) : std::string(kNewMod) });

        InstallResult result;
        if (drops.empty())
        {
            result.lines.push_back({ "Nothing to add: drop mod archives, folders or files.", true });
            return result;
        }
        // This launcher's folder in the work folder.
        const LockedFolder work(modsRoot / kWorkFolder / std::to_wstring(GetCurrentProcessId()));
        for (size_t i = 0; i < drops.size() && !stop.stop_requested(); i++)
        {
            const auto& [sources, name] = drops[i];
            const fs::path dir = work.Dir() / std::to_wstring(i);
            const std::string what = sources.size() == 1 ? Utf8(sources[0].filename()) : "the files";
            std::string error;
            std::vector<Line> notes;
            std::optional<ModInfo> mod;
            try
            {
                mod = AddOne(modsRoot, dir, sources, name, stop, game, notes, error);
            }
            catch (const fs::filesystem_error& e)
            {
                error = e.code().message(); // (what() has paths.)
            }
            catch (const std::exception& e)
            {
                error = e.what();
            }
            RemoveTree(dir);
            if (!mod)
            {
                LOGW("Mods: can't add {}: {}", what, error);
                result.lines.push_back({ fmt::format("Can't add {}: {}.", what, error), true });
                continue;
            }
            LOGI("Mods: added {} as {} ({} files)", what, mod->folder, mod->fileCount);
            if (mod->IsForPc())
                result.lines.push_back({ fmt::format("Added \"{}\", but it's made for the PC version: select it and "
                                                     "press Convert to use it.",
                                             mod->name),
                    true });
            else
                result.lines.push_back({ fmt::format("Added \"{}\" ({} file{}).", mod->name, mod->fileCount,
                    mod->fileCount == 1 ? "" : "s") });
            if (!mod->warnings.empty())
                result.lines.push_back({ fmt::format("It has {} warning{}: select it to see {}.", mod->warnings.size(),
                                             mod->warnings.size() == 1 ? "" : "s",
                                             mod->warnings.size() == 1 ? "it" : "them"),
                    true });
            result.lines.insert(result.lines.end(), notes.begin(), notes.end());
            result.mods.push_back(std::move(*mod));
        }
        return result;
    }
}
