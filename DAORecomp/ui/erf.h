#pragma once
// Reading ERF archives: the game's (V2.1, packages/*/data/*.erf) and the zlib data
// in them, and the PC version's (V2.0 and V2.2), which mods come in and the game's
// scripts' archives are in too (V2.0).

#include <variant>

namespace dao::ui
{
    inline uint32_t ReadLE32(std::span<const uint8_t> data, size_t at)
    {
        return data[at] | data[at + 1] << 8 | data[at + 2] << 16 | uint32_t(data[at + 3]) << 24;
    }

    inline uint32_t ReadBE32(std::span<const uint8_t> data, size_t at)
    {
        return uint32_t(data[at]) << 24 | data[at + 1] << 16 | data[at + 2] << 8 | data[at + 3];
    }

    // `size` bytes of a file from `offset`; nothing if it's shorter.
    std::optional<std::vector<uint8_t>> ReadFileBytes(const std::filesystem::path& path, uint64_t offset,
        uint32_t size);

    // zlib data unpacked to exactly `size` bytes, or, with `slack`, to at most `size` +
    // `slack` (for sizes that are only hints). Decoded into a buffer of that size,
    // which bounds it; nothing if that's 0.
    std::optional<std::vector<uint8_t>> Inflate(std::span<const uint8_t> packed, uint32_t size, uint32_t slack = 0);

    // An ERF V2.1 archive's directory, read once for any number of entries.
    class ErfArchive
    {
    public:
        // Entry names are at most this long.
        static constexpr size_t kNameSize = 32;

        // Nothing if it isn't an ERF V2.1 archive.
        static std::optional<ErfArchive> Open(const std::filesystem::path& path);

        struct Entry
        {
            std::string name;
            uint32_t offset, packedSize, size;
        };

        const std::vector<Entry>& Entries() const { return entries_; }

        // The (unpacked) entry `name`, if it's at most `maxSize` bytes. Names compare
        // case-insensitively.
        std::optional<std::vector<uint8_t>> Read(std::string_view name, uint32_t maxSize) const;
        std::optional<std::vector<uint8_t>> Read(const Entry& entry, uint32_t maxSize) const;

    private:
        std::filesystem::path path_;
        uint64_t fileSize_ = 0;
        std::vector<Entry> entries_;
    };

    // One entry of an archive (see ErfArchive::Read).
    std::optional<std::vector<uint8_t>> ReadErfEntry(const std::filesystem::path& archive, std::string_view name,
        uint32_t maxSize);

    // A PC archive: ERF V2.0, or V2.2 (whose entries may be compressed), with UTF-16
    // names. Its directory as written: what its names and places may be is the
    // caller's to check.
    class PcErfArchive
    {
    public:
        struct Entry
        {
            std::wstring name;
            uint32_t offset, packedSize, size;
        };

        // Or why it can't be read (to tell the player): not one of these archives,
        // more than `maxEntries` entries, damaged.
        static std::variant<PcErfArchive, std::string> Open(const std::filesystem::path& path,
            uint32_t maxEntries);

        bool IsV22() const { return v22_; }
        uint64_t FileSize() const { return fileSize_; }
        const std::vector<Entry>& Entries() const { return entries_; }

        // An entry's data (unpacked); nothing if it's damaged, outside the file or more
        // than `maxSize` bytes.
        std::optional<std::vector<uint8_t>> Read(const Entry& entry, uint32_t maxSize) const;

    private:
        std::filesystem::path path_;
        uint64_t fileSize_ = 0;
        bool v22_ = false;
        bool compressed_ = false;
        std::vector<Entry> entries_;
    };
}
