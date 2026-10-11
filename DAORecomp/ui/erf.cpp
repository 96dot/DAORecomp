#include "stdafx.h"
#include "ui/erf.h"

#include <fstream>

#include "third_party/stb/stb_image.h"

namespace fs = std::filesystem;

namespace dao::ui
{
    namespace
    {
        uint32_t Read32(std::span<const uint8_t> data, size_t at)
        {
            return data[at] | data[at + 1] << 8 | data[at + 2] << 16 | uint32_t(data[at + 3]) << 24;
        }
    }

    std::optional<std::vector<uint8_t>> ReadFileBytes(const fs::path& path, uint64_t offset, uint32_t size)
    {
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> data(size);
        if (!file.seekg(std::streamoff(offset)) ||
            !file.read(reinterpret_cast<char*>(data.data()), std::streamsize(size)))
            return std::nullopt;
        return data;
    }

    std::optional<std::vector<uint8_t>> Inflate(std::span<const uint8_t> packed, uint32_t size, uint32_t slack)
    {
        const uint64_t capacity = uint64_t(size) + slack;
        if (packed.size() > INT_MAX || capacity > INT_MAX || capacity == 0)
            return std::nullopt;
        std::vector<uint8_t> out(static_cast<size_t>(capacity));
        const int length = stbi_zlib_decode_buffer(reinterpret_cast<char*>(out.data()), int(out.size()),
            reinterpret_cast<const char*>(packed.data()), int(packed.size()));
        if (length <= 0 || (!slack && uint32_t(length) != size))
            return std::nullopt;
        out.resize(size_t(length));
        return out;
    }

    // ERF V2.1: "ERF V2.1", the entry count at 0x10, then 44-byte entries from 0x20:
    // name[32], offset, packed size, size. An entry is zlib-compressed when its sizes
    // differ.
    std::optional<std::vector<uint8_t>> ReadErfEntry(const fs::path& archive, std::string_view name,
        uint32_t maxSize)
    {
        constexpr size_t kEntriesOffset = 0x20;
        constexpr size_t kEntrySize = 44;
        constexpr size_t kNameSize = 32;
        // A sanity bound (the game's archives have far fewer entries).
        constexpr uint32_t kMaxEntries = 1u << 16;

        std::error_code ec;
        const uint64_t fileSize = fs::file_size(archive, ec);
        const auto header = ec ? std::nullopt : ReadFileBytes(archive, 0, kEntriesOffset);
        if (!header || memcmp(header->data(), "ERF V2.1", 8) != 0)
            return std::nullopt;
        const uint32_t count = Read32(*header, 0x10);
        if (count > kMaxEntries)
            return std::nullopt;
        const auto entries = ReadFileBytes(archive, kEntriesOffset, uint32_t(count * kEntrySize));
        if (!entries)
            return std::nullopt;
        for (uint32_t i = 0; i < count; ++i)
        {
            const std::span<const uint8_t> entry(entries->data() + i * kEntrySize, kEntrySize);
            const auto* entryName = reinterpret_cast<const char*>(entry.data());
            const std::string_view entryView(entryName, strnlen(entryName, kNameSize));
            if (!std::ranges::equal(entryView, name,
                    [](char a, char b) { return tolower(uint8_t(a)) == tolower(uint8_t(b)); }))
                continue;
            const uint32_t offset = Read32(entry, kNameSize), packed = Read32(entry, kNameSize + 4),
                           size = Read32(entry, kNameSize + 8);
            if (packed > maxSize || size > maxSize || uint64_t(offset) + packed > fileSize)
                return std::nullopt;
            auto data = ReadFileBytes(archive, offset, packed);
            if (!data || packed == size)
                return data;
            return Inflate(*data, size);
        }
        return std::nullopt;
    }
}
