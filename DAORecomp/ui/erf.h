#pragma once
// Reading the game's files: its ERF V2.1 archives (packages/*/data/*.erf) and the
// zlib data in them.

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

        // The (unpacked) entry `name`, if it's at most `maxSize` bytes. Names compare
        // case-insensitively.
        std::optional<std::vector<uint8_t>> Read(std::string_view name, uint32_t maxSize) const;

    private:
        struct Entry
        {
            std::string name;
            uint32_t offset, packedSize, size;
        };

        std::filesystem::path path_;
        uint64_t fileSize_ = 0;
        std::vector<Entry> entries_;
    };

    // One entry of an archive (see ErfArchive::Read).
    std::optional<std::vector<uint8_t>> ReadErfEntry(const std::filesystem::path& archive, std::string_view name,
        uint32_t maxSize);
}
