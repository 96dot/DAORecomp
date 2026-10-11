#include "stdafx.h"
#include "ui/erf.h"

#include <fstream>

#include "third_party/stb/stb_image.h"

namespace fs = std::filesystem;

namespace dao::ui
{
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
    std::optional<ErfArchive> ErfArchive::Open(const fs::path& path)
    {
        constexpr size_t kEntriesOffset = 0x20;
        constexpr size_t kEntrySize = 44;
        // A sanity bound (the game's archives have far fewer entries).
        constexpr uint32_t kMaxEntries = 1u << 16;

        std::error_code ec;
        const uint64_t fileSize = fs::file_size(path, ec);
        const auto header = ec ? std::nullopt : ReadFileBytes(path, 0, kEntriesOffset);
        if (!header || memcmp(header->data(), "ERF V2.1", 8) != 0)
            return std::nullopt;
        const uint32_t count = ReadLE32(*header, 0x10);
        if (count > kMaxEntries)
            return std::nullopt;
        const auto table = ReadFileBytes(path, kEntriesOffset, uint32_t(count * kEntrySize));
        if (!table)
            return std::nullopt;

        ErfArchive archive;
        archive.path_ = path;
        archive.fileSize_ = fileSize;
        archive.entries_.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const std::span<const uint8_t> entry(table->data() + i * kEntrySize, kEntrySize);
            const auto* name = reinterpret_cast<const char*>(entry.data());
            archive.entries_.push_back(Entry{ std::string(name, strnlen(name, kNameSize)),
                ReadLE32(entry, kNameSize), ReadLE32(entry, kNameSize + 4), ReadLE32(entry, kNameSize + 8) });
        }
        return archive;
    }

    std::optional<std::vector<uint8_t>> ErfArchive::Read(std::string_view name, uint32_t maxSize) const
    {
        for (const Entry& entry : entries_)
        {
            if (!std::ranges::equal(entry.name, name,
                    [](char a, char b) { return tolower(uint8_t(a)) == tolower(uint8_t(b)); }))
                continue;
            if (entry.packedSize > maxSize || entry.size > maxSize ||
                uint64_t(entry.offset) + entry.packedSize > fileSize_)
                return std::nullopt;
            auto data = ReadFileBytes(path_, entry.offset, entry.packedSize);
            if (!data || entry.packedSize == entry.size)
                return data;
            return Inflate(*data, entry.size);
        }
        return std::nullopt;
    }

    std::optional<std::vector<uint8_t>> ReadErfEntry(const fs::path& archive, std::string_view name, uint32_t maxSize)
    {
        const auto erf = ErfArchive::Open(archive);
        return erf ? erf->Read(name, maxSize) : std::nullopt;
    }
}
