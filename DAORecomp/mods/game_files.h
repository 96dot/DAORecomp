#pragma once
// The names of the game's files that mods replace by name: those in the archives
// under packages/core/data (models, textures, materials, tables, ...) and the
// campaign's under modules/<module>/data (areas, scripts, dialogs, ...).

#include <stop_token>
#include <unordered_map>

namespace dao::mods
{
    class GameFiles
    {
    public:
        // Reads the archives' directories (not their files): a fraction of a second.
        static GameFiles Read(const std::filesystem::path& gameRoot, std::stop_token stop = {});

        // Whether the game has a file of this name (any case).
        bool Has(std::string_view name) const;
        bool Empty() const { return names_.empty(); }

        // The game's file of this name, if it's at most `maxSize` bytes (not those in
        // the scripts' archives).
        std::optional<std::vector<uint8_t>> ReadFile(std::string_view name, uint32_t maxSize) const;

    private:
        std::vector<std::filesystem::path> archives_;
        // Lower case, with the index of the archive to read it from (kNoArchive: one
        // that can't be read).
        static constexpr uint32_t kNoArchive = ~0u;
        std::unordered_map<std::string, uint32_t> names_;
    };
}
