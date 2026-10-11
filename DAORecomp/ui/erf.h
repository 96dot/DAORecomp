#pragma once
// Reading the game's files: its ERF V2.1 archives (packages/*/data/*.erf) and the
// zlib data in them.

namespace dao::ui
{
    // `size` bytes of a file from `offset`; nothing if it's shorter.
    std::optional<std::vector<uint8_t>> ReadFileBytes(const std::filesystem::path& path, uint64_t offset,
        uint32_t size);

    // zlib data unpacked to exactly `size` bytes, or, with `slack`, to at most `size` +
    // `slack` (for sizes that are only hints). Decoded into a buffer of that size,
    // which bounds it.
    std::optional<std::vector<uint8_t>> Inflate(std::span<const uint8_t> packed, uint32_t size, uint32_t slack = 0);

    // The (unpacked) entry `name` of an ERF V2.1 archive, if it's at most `maxSize`
    // bytes. Names compare case-insensitively.
    std::optional<std::vector<uint8_t>> ReadErfEntry(const std::filesystem::path& archive, std::string_view name,
        uint32_t maxSize);
}
