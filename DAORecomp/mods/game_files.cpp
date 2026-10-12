#include "stdafx.h"
#include "mods/game_files.h"

#include "text.h"
#include "ui/erf.h"

namespace dao::mods
{
    namespace fs = std::filesystem;

    namespace
    {
        // The entry names of an ERF V2.0 archive (the scripts' archives are in that
        // format, as on the PC).
        void ReadErf20Names(const fs::path& path, std::unordered_map<std::string, uint32_t>& names, uint32_t archive)
        {
            // More than any of the game's archives has.
            constexpr uint32_t kMaxEntries = 1 << 20;
            const auto erf = ui::PcErfArchive::Open(path, kMaxEntries);
            const auto* opened = std::get_if<ui::PcErfArchive>(&erf);
            // (V2.2 is the PC's DLC format, which the game's own files aren't in.)
            if (!opened || opened->IsV22())
                return;
            for (const ui::PcErfArchive::Entry& entry : opened->Entries())
            {
                std::string name;
                // The game's names are ASCII.
                for (const wchar_t c : entry.name)
                    name += c < 0x80 ? char(c) : '?';
                names.emplace(Lower(name), archive);
            }
        }
    }

    GameFiles GameFiles::Read(const fs::path& gameRoot, std::stop_token stop)
    {
        GameFiles files;
        // The core's first: where a name is in both, it's read from there.
        std::vector<fs::path> folders = { gameRoot / "packages" / "core" / "data" };
        std::error_code ec;
        for (const auto& module : fs::directory_iterator(gameRoot / "modules", ec))
            folders.push_back(module.path() / "data");
        for (const fs::path& data : folders)
        {
            for (auto it = fs::recursive_directory_iterator(data, ec);
                 !ec && it != fs::recursive_directory_iterator() && !stop.stop_requested(); it.increment(ec))
            {
                const std::string extension = Lower(Utf8(it->path().extension()));
                if (extension != ".erf" && extension != ".rim")
                    continue;
                if (const auto erf = ui::ErfArchive::Open(it->path()))
                {
                    files.archives_.push_back(it->path());
                    const uint32_t archive = uint32_t(files.archives_.size() - 1);
                    for (const auto& entry : erf->Entries())
                        files.names_.emplace(Lower(entry.name), archive);
                }
                else
                    ReadErf20Names(it->path(), files.names_, kNoArchive);
            }
        }
        return files;
    }

    bool GameFiles::Has(std::string_view name) const
    {
        return names_.contains(Lower(std::string(name)));
    }

    std::optional<std::vector<uint8_t>> GameFiles::ReadFile(std::string_view name, uint32_t maxSize) const
    {
        const auto it = names_.find(Lower(std::string(name)));
        if (it == names_.end() || it->second == kNoArchive)
            return std::nullopt;
        return ui::ReadErfEntry(archives_[it->second], name, maxSize);
    }
}
