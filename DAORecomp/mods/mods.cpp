#include "stdafx.h"
#include "mods/mods.h"

#include <shellapi.h>

#ifndef DAO_LAUNCHER_ONLY
#include "kernel/vfs.h"
#endif
#include "mods/mod_files.h"
#include "text.h"

#include "xenia/base/cvar.h" // toml++

namespace dao::mods
{
    namespace fs = std::filesystem;

    namespace
    {
        // A bigger mod.toml isn't one (it's read on every start).
        constexpr uintmax_t kMaxModTomlBytes = 1 << 20;
        // Enough of a file to tell its format: a GFF or archive header, or an FSB4
        // sound bank's header and its first sample's (whose mode is at 0x60).
        constexpr size_t kHeadBytes = 0x64;
        // In an FSB4 sample's mode: the sample is XMA, as all of the 360 version's are.
        constexpr uint32_t kFsbXma = 0x01000000;
        // A texture this big is more than 2048x2048 (with its mip levels), the most the
        // game's own are; this many megabytes of them is worth a warning.
        constexpr uintmax_t kLargeTextureBytes = 6 << 20;
        constexpr uintmax_t kLargeTexturesWarnBytes = 128 << 20;

        // The highest engine function (script command) the 360 version's own scripts
        // call, of all of them: a script calling a higher one was compiled for a later
        // PC patch, and the 360 version may not have it.
        constexpr uint16_t kMaxGameScriptFunction = 871;
        // A bigger script isn't one.
        constexpr uintmax_t kMaxScriptBytes = 4 << 20;

        // The engine functions a compiled script calls, or nothing if it can't be
        // decoded. Its format: "NCS V1.0B", the size (big-endian), then NWN's
        // instructions (an opcode, a type, operands) and a few of Dragon Age's.
        std::optional<std::vector<uint16_t>> ScriptCalls(std::span<const uint8_t> code)
        {
            // Each opcode's operand bytes (-1: unknown); CONST, ACTION and EQUAL/NEQUAL vary.
            static constexpr int8_t kOperands[] = {
                -1, 6, 0, 6, -1, 3, 0, 0, 0, 0, 0, -1, -1, 0, 0, 0,   // 0x00
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 4, 4, 4,       // 0x10
                0, 6, 0, 4, 4, 4, 6, 6, 4, 4, 0, 0, 8, 0, 0, -1,      // 0x20
                6, -1, 6,                                             // 0x30
            };
            constexpr uint8_t kConst = 0x04, kAction = 0x05, kEqual = 0x0B, kNotEqual = 0x0C;
            constexpr uint8_t kString = 0x05, kResource = 0x60, kInt = 0x03, kFloat = 0x04, kObject = 0x06;
            constexpr uint8_t kStructs = 0x24;
            constexpr size_t kHeader = 13;
            auto be16 = [&](size_t at) { return uint16_t(code[at] << 8 | code[at + 1]); };
            if (code.size() < kHeader || memcmp(code.data(), "NCS V1.0B", 9) != 0)
                return std::nullopt;
            const size_t end = size_t(code[9]) << 24 | code[10] << 16 | code[11] << 8 | code[12];
            if (end > code.size())
                return std::nullopt;
            std::vector<uint16_t> calls;
            for (size_t at = kHeader; at < end;)
            {
                if (at + 2 > end)
                    return std::nullopt;
                const uint8_t op = code[at], type = code[at + 1];
                at += 2;
                size_t operands;
                if (op == kConst && (type == kString || type == kResource))
                    operands = at + 2 <= end ? 2 + be16(at) : SIZE_MAX;
                else if (op == kConst)
                    operands = type == kInt || type == kFloat || type == kObject ? 4 : SIZE_MAX;
                else if (op == kEqual || op == kNotEqual)
                    operands = type == kStructs ? 2 : 0;
                else if (op < std::size(kOperands) && kOperands[op] >= 0)
                    operands = size_t(kOperands[op]);
                else
                    return std::nullopt;
                if (operands > end - at)
                    return std::nullopt;
                if (op == kAction)
                    calls.push_back(be16(at));
                at += operands;
            }
            return calls;
        }

        // Whether a file is in a format of the PC version: a GFF file (models,
        // meshes, morphs, ...) for the PC platform, a DDS texture (the Xbox 360
        // version's are .xds) or a PC archive (its header in UTF-16).
        bool IsPcFile(std::string_view extension, std::span<const uint8_t> head)
        {
            if (extension == ".dds" || extension == ".dazip")
                return true;
            return IsPcGff(head) || IsPcArchive(head);
        }

        // Files whose first bytes don't matter: those in formats that are the same
        // on both versions (or are only the 360's), which aren't GFF files or archives.
        bool HeadTellsNothing(std::string_view extension)
        {
            static constexpr std::string_view kKnown[] = { ".xds", ".dds", ".ncs", ".nss", ".gfx", ".mao", ".xml",
                ".tga", ".bik", ".wav", ".ogg", ".toml", ".dazip" };
            return std::ranges::find(kKnown, extension) != std::end(kKnown);
        }

        // A sound bank the 360 version can't play: one not in its format (FSB4 with
        // XMA samples).
        bool IsPcSoundBank(std::span<const uint8_t> head)
        {
            if (head.size() < kHeadBytes || memcmp(head.data(), "FSB4", 4) != 0)
                return true;
            uint32_t mode;
            memcpy(&mode, head.data() + 0x60, sizeof(mode));
            return !(mode & kFsbXma);
        }

        // Where the game finds a file of a mod (see ModInfo::changes).
        std::string ChangeOf(const fs::path& relative)
        {
            if (IsUnder(relative.parent_path(), "override"))
                return "override/" + Lower(Utf8(relative.filename()));
            return Lower(WideToUtf8(relative.generic_wstring()));
        }

        bool IsProgram(std::string_view extension)
        {
            static constexpr std::string_view kPrograms[] = { ".exe", ".dll", ".asi", ".bat", ".cmd", ".ps1",
                ".msi", ".jar" };
            return std::ranges::find(kPrograms, extension) != std::end(kPrograms);
        }

        // Files of one kind in a mod: how many, the first's path and their size.
        struct Found
        {
            size_t count = 0;
            std::string example;
            uintmax_t bytes = 0;

            void Add(const std::string& path, uintmax_t size = 0)
            {
                if (count++ == 0)
                    example = path;
                bytes += size;
            }

            // "x.gda" or "3 files such as x.gda".
            std::string Some() const
            {
                return count == 1 ? example : fmt::format("{} files such as {}", count, example);
            }
        };

        struct Findings
        {
            Found menus, sounds, newScripts, oddScripts, gameTables, addins, programs, outside, largeTextures, twice;
            // The highest engine function a script in newScripts calls.
            uint16_t newFunction = 0;

            std::vector<std::string> Warnings() const
            {
                std::vector<std::string> out;
                if (menus.count)
                    out.push_back(fmt::format("Changes menus ({}). The PC version's menus differ from the 360 "
                                              "version's, so these may not show or may stop the game.",
                        menus.Some()));
                if (sounds.count)
                    out.push_back(fmt::format("Has sound banks made for the PC version ({}), which this version "
                                              "can't play: those sounds will be silent.",
                        sounds.Some()));
                if (newScripts.count)
                    out.push_back(fmt::format("Has scripts ({}) that use game commands the 360 version's own never "
                                              "do (up to #{}), so may be newer than it: they may stop partway, "
                                              "which can break quests.",
                        newScripts.Some(), newFunction));
                if (oddScripts.count)
                    out.push_back(fmt::format("Has scripts ({}) that aren't compiled as the game's are, so may not "
                                              "run.",
                        oddScripts.Some()));
                if (gameTables.count)
                    out.push_back(fmt::format("Replaces whole game tables ({}) with its own, which may differ from "
                                              "the 360 version's and undo other mods' changes to them.",
                        gameTables.Some()));
                if (addins.count)
                    out.push_back(fmt::format("Has add-in content ({}): new areas or a campaign, which the 360 "
                                              "version can't load.",
                        addins.Some()));
                if (programs.count)
                    out.push_back(fmt::format("Comes with programs ({}), which are for the PC version and don't "
                                              "run with this one.",
                        programs.Some()));
                if (outside.count)
                    out.push_back(fmt::format("Has files outside the game's folders ({}), which the game won't see. "
                                              "Mod files go in packages/core/override/.",
                        outside.Some()));
                if (largeTextures.bytes >= kLargeTexturesWarnBytes)
                    out.push_back(fmt::format("Has {} very large textures ({} MB). The 360 version has 512 MB of "
                                              "memory for everything, so the game may run out and crash.",
                        largeTextures.count, largeTextures.bytes >> 20));
                if (twice.count)
                    out.push_back(fmt::format("Has files twice in different folders ({}), of which the game uses "
                                              "only one each. If the mod has optional parts, keep only those "
                                              "you want.",
                        twice.Some()));
                return out;
            }
        };
    }

    bool IsInAddinModule(const fs::path& relative)
    {
        std::vector<std::string> parts;
        for (const auto& part : relative)
            parts.push_back(Lower(Utf8(part)));
        for (size_t i = 0; i + 2 < parts.size(); ++i)
            if (parts[i] == "addins" && parts[i + 2] == "module")
                return true;
        return false;
    }

    namespace
    {
        // The mod's description from its mod.toml, if it has one.
        void ReadModToml(const fs::path& manifest, ModInfo& info)
        {
            std::error_code ec;
            if (!fs::is_regular_file(manifest, ec))
                return;
            const auto bytes = ReadAll(manifest, kMaxModTomlBytes);
            if (!bytes)
            {
                LOGW("Mod {}: mod.toml can't be read or is too big to be one", info.folder);
                return;
            }
            try
            {
                const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                const toml::table table = toml::parse(text, Utf8(manifest));
                info.name = table["name"].value_or(info.name);
                info.version = table["version"].value_or(std::string());
                info.author = table["author"].value_or(std::string());
                info.description = table["description"].value_or(std::string());
            }
            catch (const toml::parse_error& error)
            {
                LOGW("Mod {}: mod.toml: {}", info.folder, error.description());
            }
        }
    }

    ModInfo Describe(const fs::path& dir, const GameFiles* game, Detail detail)
    {
        ModInfo info;
        info.folder = Utf8(dir.filename());
        info.name = info.folder;
        const auto manifest = dir / "mod.toml";
        ReadModToml(manifest, info);

        const bool full = detail == Detail::Full;
        Findings found;
        std::unordered_set<std::string> changes;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
            // (A file that can't be looked at doesn't end the listing.)
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc) || it->path() == manifest)
                continue;
            info.fileCount++;
            const fs::path relative = it->path().lexically_relative(dir);
            const std::string extension = Lower(Utf8(relative.extension()));
            const bool document = IsDocument(relative);
            // (Loading needs only whether it's for the PC version, not the sound banks' heads.)
            const bool needsHead = !document && !HeadTellsNothing(extension) && (full || extension != ".fsb");
            const auto head = needsHead ? ReadHead(it->path(), kHeadBytes) : std::vector<uint8_t>();
            if (IsPcFile(extension, head) && info.pcFiles++ == 0)
            {
                info.pcExample = WideToUtf8(relative.generic_wstring());
                if (!full)
                    break;
            }
            if (document || !full)
                continue;
            const std::string path = WideToUtf8(relative.generic_wstring());
            const std::string name = Lower(Utf8(relative.filename()));
            const std::string top = Lower(Utf8(*relative.begin()));
            if (!relative.has_parent_path() || (top != "packages" && top != "modules"))
                found.outside.Add(path);
            if (extension == ".gfx")
                found.menus.Add(path);
            else if (extension == ".fsb" && IsPcSoundBank(head))
                found.sounds.Add(path);
            else if (extension == ".ncs")
            {
                const auto code = ReadAll(it->path(), kMaxScriptBytes);
                const auto calls = code ? ScriptCalls(*code) : std::nullopt;
                if (!calls)
                    found.oddScripts.Add(path);
                else if (const uint16_t newest = calls->empty() ? 0 : std::ranges::max(*calls);
                         newest > kMaxGameScriptFunction)
                {
                    found.newScripts.Add(path);
                    found.newFunction = std::max(found.newFunction, newest);
                }
            }
            else if (extension == ".gda" && game && game->Has(name))
                found.gameTables.Add(path);
            else if (IsProgram(extension))
                found.programs.Add(path);
            if (IsInAddinModule(relative))
                found.addins.Add(path);
            if (extension == ".xds" || extension == ".dds")
            {
                if (const uintmax_t size = it->file_size(fileEc); !fileEc && size >= kLargeTextureBytes)
                    found.largeTextures.Add(path, size);
            }

            std::string change = ChangeOf(relative);
            if (!changes.insert(change).second)
                found.twice.Add(path);
            else
                info.changes.push_back(std::move(change));
        }
        if (!full)
        {
            if (info.IsForPc())
                LOGW("Mod {}: made for the PC version (such as {}); not loaded", info.folder, info.pcExample);
            return info;
        }
        info.warnings = found.Warnings();
        if (info.IsForPc())
            LOGW("Mod {}: made for the PC version ({} files such as {}); not loaded", info.folder, info.pcFiles,
                info.pcExample);
        for (const std::string& warning : info.warnings)
            LOGI("Mod {}: {}", info.folder, warning);
        return info;
    }

    fs::path ModsRoot(const fs::path& gameRoot)
    {
        return gameRoot.parent_path() / "mods";
    }

    std::vector<ModInfo> Scan(const fs::path& modsRoot, const std::vector<ModState>& saved, const GameFiles* game,
        Detail detail)
    {
        std::error_code ec;
        fs::create_directories(modsRoot, ec);

        std::vector<fs::path> folders;
        for (const auto& entry : fs::directory_iterator(modsRoot, ec))
        {
            // Not the hidden work folder of mods being added.
            if (entry.is_directory(ec) && !entry.path().filename().native().starts_with(kHiddenPrefix))
                folders.push_back(entry.path());
        }
        std::sort(folders.begin(), folders.end());

        std::vector<ModInfo> out;
        for (const ModState& state : saved)
        {
            const auto it = std::find_if(folders.begin(), folders.end(),
                [&](const fs::path& p) { return _stricmp(Utf8(p.filename()).c_str(), state.folder.c_str()) == 0; });
            if (it == folders.end())
                continue;
            ModInfo info = Describe(*it, game, detail);
            info.enabled = state.enabled;
            out.push_back(std::move(info));
            folders.erase(it);
        }
        for (const auto& folder : folders)
            out.push_back(Describe(folder, game, detail));
        if (detail == Detail::Full)
            FindOverlaps(out);
        return out;
    }

    std::vector<ModState> ToState(const std::vector<ModInfo>& mods)
    {
        std::vector<ModState> out;
        for (const ModInfo& mod : mods)
            out.push_back({ mod.folder, mod.enabled });
        return out;
    }

    void FindOverlaps(std::vector<ModInfo>& mods)
    {
        // Each change: the loaded mods making it.
        std::unordered_map<std::string_view, std::vector<size_t>> changers;
        for (size_t i = 0; i < mods.size(); ++i)
        {
            if (mods[i].IsLoaded())
                for (const std::string& change : mods[i].changes)
                    changers[change].push_back(i);
        }
        for (size_t i = 0; i < mods.size(); ++i)
        {
            ModInfo& mod = mods[i];
            mod.overlap.clear();
            if (!mod.IsLoaded())
                continue;
            // The other mods in load order, with how many of this one's files each changes.
            std::map<size_t, size_t> shared;
            std::string example;
            for (const std::string& change : mod.changes)
            {
                const auto& with = changers.find(change)->second;
                if (with.size() > 1 && example.empty())
                    example = change.substr(change.rfind('/') + 1);
                for (const size_t other : with)
                    if (other != i)
                        shared[other]++;
            }
            if (shared.empty())
                continue;
            std::string others;
            for (const auto& [other, count] : shared)
                others += fmt::format("{}{} ({} files)", others.empty() ? "" : ", ", mods[other].name, count);
            mod.overlap = fmt::format("Changes the same files as {} (such as {}). The game uses only one mod's "
                                      "file each: where they're in the same folder, the one lower in the list.",
                others, example);
            LOGI("Mod {}: changes the same files as {}", mod.folder, others);
        }
    }

    bool RecycleMod(const fs::path& modsRoot, const std::string& folder, std::string& error)
    {
        // The paths, each ending in a null, the list in another. A link (which the
        // mods folder may have been given by hand) goes alone, not what it leads to.
        std::wstring from;
        std::error_code ec;
        for (const fs::path& path : { modsRoot / FromUtf8(folder), modsRoot / kOriginalsFolder / FromUtf8(folder) })
        {
            if (IsLink(path))
                fs::remove(path, ec);
            else if (fs::exists(path, ec))
                from += path.native() + L'\0';
        }
        if (from.empty())
            return true;
        from += L'\0';
        SHFILEOPSTRUCTW operation = {};
        operation.wFunc = FO_DELETE;
        operation.pFrom = from.c_str();
        // To the Recycle Bin, without asking, except before deleting for good.
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT;
        if (const int result = SHFileOperationW(&operation); result != 0 || operation.fAnyOperationsAborted)
        {
            error = result != 0 ? fmt::format("Windows couldn't move it (error {:#x})", unsigned(result))
                                : "it was kept";
            LOGW("Mods: can't delete {}: {}", folder, error);
            return false;
        }
        LOGI("Mods: deleted {} (to the Recycle Bin)", folder);
        fs::remove(modsRoot / kOriginalsFolder, ec); // Only if empty.
        return true;
    }

#ifndef DAO_LAUNCHER_ONLY
    // (The game's: the launcher-only build has no file system to give the mods to.)
    void Apply(const fs::path& modsRoot, const std::vector<ModInfo>& mods)
    {
        // Highest priority first: the last mod in the load order wins.
        std::vector<fs::path> overlays;
        for (auto it = mods.rbegin(); it != mods.rend(); ++it)
        {
            std::error_code ec;
            const fs::path folder = modsRoot / FromUtf8(it->folder);
            if (it->IsLoaded() && fs::is_directory(folder, ec))
                overlays.push_back(folder);
        }
        LOGI("Mods: {} enabled", overlays.size());
        kernel::VfsSetOverlays("\\Device\\Cdrom0", std::move(overlays));
    }
#endif
}
