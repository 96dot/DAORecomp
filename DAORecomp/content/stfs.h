#pragma once
// Xbox 360 STFS packages in the read-only format Xbox Live delivers them in
// (LIVE/PIRS: downloadable content and title updates), after Xenia's reader. The
// packages are the player's files and treated as untrusted: every block, name and
// size is checked first.
//
// scripts/stfs.py is the same reader in Python, for the installer (which runs
// before any of this is built); keep the two in step.

#include <stop_token>

#include "content/content.h"

namespace dao::content
{
    struct StfsFile
    {
        std::string path;   // '/'-separated, names checked to be valid on Windows
        uint64_t size = 0;
        uint32_t startBlock = 0;
        bool contiguous = false;
    };

    class StfsPackage
    {
    public:
        ~StfsPackage();
        StfsPackage(const StfsPackage&) = delete;
        StfsPackage& operator=(const StfsPackage&) = delete;

        // Whether a file starts like a package (CON, LIVE or PIRS).
        static bool IsPackage(const std::filesystem::path& path);
        // Opens a package and reads its file list; nullptr and why in `error` if
        // it isn't one this can read, or is damaged.
        static std::unique_ptr<StfsPackage> Open(const std::filesystem::path& path, std::string& error);

        uint32_t titleId = 0;
        uint32_t contentType = 0;
        uint32_t mediaId = 0;
        std::string contentId;     // the header's SHA-1 (hex), which the console names packages by
        std::u16string displayName;
        std::vector<StfsFile> files;

        // Extracts every file into `into`, an empty folder. false (and why) on an
        // error or when `stop` is requested; what's written stays.
        bool Extract(const std::filesystem::path& into, std::stop_token stop, std::string& error);

    private:
        StfsPackage() = default;
        bool ReadHeader(std::string& error);
        bool ReadTable(std::string& error);
        uint64_t BlockOffset(uint32_t block) const;
        bool ReadAt(uint64_t offset, void* buffer, size_t size);
        bool ReadBlock(uint32_t block, void* buffer, size_t size, std::string& error);
        bool NextBlock(uint32_t block, uint32_t& next, std::string& error);
        bool ReadRun(uint32_t block, uint32_t count, uint8_t* buffer, size_t size, std::string& error);

        HANDLE m_file = INVALID_HANDLE_VALUE;
        uint64_t m_size = 0;
        uint64_t m_base = 0;
        uint32_t m_tableBlocks = 0;
        uint32_t m_tableBlock = 0;
        uint32_t m_totalBlocks = 0;
        // The hash table NextBlock read last (a chain stays in one for 170 blocks).
        std::vector<uint8_t> m_hashTable;
        uint64_t m_hashTableAt = UINT64_MAX;
    };
}
