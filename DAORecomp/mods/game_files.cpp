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
        // format, as on the PC): "ERF V2.0" in UTF-16, the entry count at 16, then
        // 72-byte entries from 32: the name (32 UTF-16 characters), offset, size.
        void ReadErf20Names(const fs::path& path, std::unordered_map<std::string, uint32_t>& names, uint32_t archive)
        {
            constexpr size_t kEntriesOffset = 32, kEntrySize = 72, kNameBytes = 64;
            // More than any of the game's archives has.
            constexpr uint32_t kMaxEntries = 1 << 20;
            const auto header = ui::ReadFileBytes(path, 0, kEntriesOffset);
            if (!header || memcmp(header->data(), "E\0R\0F\0 \0V\0" "2\0.\0" "0\0", 16) != 0)
                return;
            const uint32_t count = ui::ReadLE32(*header, 16);
            if (count > kMaxEntries)
                return;
            const auto entries = ui::ReadFileBytes(path, kEntriesOffset, count * uint32_t(kEntrySize));
            if (!entries)
                return;
            for (uint32_t i = 0; i < count; ++i)
            {
                std::string name;
                for (size_t at = i * kEntrySize; at < i * kEntrySize + kNameBytes; at += 2)
                {
                    const uint16_t c = (*entries)[at] | (*entries)[at + 1] << 8;
                    if (c == 0)
                        break;
                    // The game's names are ASCII.
                    name += c < 0x80 ? char(c) : '?';
                }
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
