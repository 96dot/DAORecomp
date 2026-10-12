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
        // (One open for both reads: the game has hundreds of archives.)
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> header(kEntriesOffset);
        if (ec || !file.read(reinterpret_cast<char*>(header.data()), std::streamsize(header.size())) ||
            memcmp(header.data(), "ERF V2.1", 8) != 0)
            return std::nullopt;
        const uint32_t count = ReadLE32(header, 0x10);
        if (count > kMaxEntries)
            return std::nullopt;
        std::vector<uint8_t> table(size_t(count) * kEntrySize);
        if (!file.read(reinterpret_cast<char*>(table.data()), std::streamsize(table.size())))
            return std::nullopt;

        ErfArchive archive;
        archive.path_ = path;
        archive.fileSize_ = fileSize;
        archive.entries_.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const std::span<const uint8_t> entry(table.data() + i * kEntrySize, kEntrySize);
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
            if (std::ranges::equal(entry.name, name,
                    [](char a, char b) { return tolower(uint8_t(a)) == tolower(uint8_t(b)); }))
                return Read(entry, maxSize);
        }
        return std::nullopt;
    }

    std::optional<std::vector<uint8_t>> ErfArchive::Read(const Entry& entry, uint32_t maxSize) const
    {
        if (entry.packedSize > maxSize || entry.size > maxSize || uint64_t(entry.offset) + entry.packedSize > fileSize_)
            return std::nullopt;
        auto data = ReadFileBytes(path_, entry.offset, entry.packedSize);
        if (!data || entry.packedSize == entry.size)
            return data;
        return Inflate(*data, entry.size);
    }

    std::optional<std::vector<uint8_t>> ReadErfEntry(const fs::path& archive, std::string_view name, uint32_t maxSize)
    {
        const auto erf = ErfArchive::Open(archive);
        return erf ? erf->Read(name, maxSize) : std::nullopt;
    }

    // ERF V2.0 and V2.2: the version in UTF-16, the entry count at 16, then entries
    // from 32 (V2.2: flags at 32, entries from 56): the name (32 UTF-16 characters),
    // offset, packed size, then (V2.2) size.
    std::variant<PcErfArchive, std::string> PcErfArchive::Open(const fs::path& path, uint32_t maxEntries)
    {
        constexpr size_t kHeaderSize = 56;
        constexpr size_t kNameChars = 32;
        std::error_code ec;
        const uint64_t fileSize = fs::file_size(path, ec);
        if (ec)
            return std::string("it can't be read");
        const auto header = ReadFileBytes(path, 0, uint32_t(std::min<uint64_t>(fileSize, kHeaderSize)));
        if (!header || header->size() < 32)
            return std::string("it's damaged");
        std::string magic;
        for (size_t i = 0; i < 8; ++i)
            magic += (*header)[2 * i + 1] == 0 ? char((*header)[2 * i]) : '?';
        if (magic == "ERF V3.0")
            return std::string("it's made for Dragon Age II");
        const bool v22 = magic == "ERF V2.2";
        if (magic != "ERF V2.0" && !v22)
            return std::string("it isn't an archive this can read");
        PcErfArchive archive;
        archive.path_ = path;
        archive.fileSize_ = fileSize;
        archive.v22_ = v22;
        const uint32_t count = ReadLE32(*header, 16);
        if (v22)
        {
            if (header->size() < kHeaderSize)
                return std::string("it's damaged");
            const uint32_t flags = ReadLE32(*header, 32);
            if ((flags >> 4) & 0xF)
                return std::string("it's encrypted (downloadable content)");
            archive.compressed_ = (flags >> 29) != 0;
        }
        const size_t entriesAt = v22 ? 56 : 32, entrySize = v22 ? 76 : 72;
        if (count > maxEntries)
            return std::string("it's damaged");
        const auto table = ReadFileBytes(path, entriesAt, uint32_t(count * entrySize));
        if (!table)
            return std::string("it's damaged");
        for (uint32_t i = 0; i < count; ++i)
        {
            const std::span<const uint8_t> raw(table->data() + i * entrySize, entrySize);
            std::wstring name;
            for (size_t c = 0; c < kNameChars && (raw[2 * c] || raw[2 * c + 1]); ++c)
                name += wchar_t(raw[2 * c] | raw[2 * c + 1] << 8);
            Entry entry{ std::move(name), ReadLE32(raw, 64), ReadLE32(raw, 68), 0 };
            entry.size = v22 ? ReadLE32(raw, 72) : entry.packedSize;
            archive.entries_.push_back(std::move(entry));
        }
        return archive;
    }

    std::optional<std::vector<uint8_t>> PcErfArchive::Read(const Entry& entry, uint32_t maxSize) const
    {
        if (entry.size > maxSize || entry.packedSize > maxSize)
            return std::nullopt;
        auto data = ReadFileBytes(path_, entry.offset, entry.packedSize);
        if (!data || !compressed_ || entry.size == 0)
            return data;
        // Deflate packs at most about 1032 to 1: more is damage (and an
        // allocation for nothing).
        if (entry.packedSize == 0 || entry.size / entry.packedSize > 1032)
            return std::nullopt;
        // BioWare's zlib (a byte, then a raw deflate stream), a raw stream or a
        // zlib one, whichever unpacks to the size.
        std::vector<uint8_t> out(entry.size);
        const auto* in = reinterpret_cast<const char*>(data->data());
        auto* dst = reinterpret_cast<char*>(out.data());
        const int size = int(entry.size), packed = int(data->size());
        if (stbi_zlib_decode_buffer(dst, size, in, packed) == size ||
            stbi_zlib_decode_noheader_buffer(dst, size, in, packed) == size ||
            (packed > 1 && stbi_zlib_decode_noheader_buffer(dst, size, in + 1, packed - 1) == size))
            return out;
        // Stored as it is.
        if (entry.packedSize == entry.size)
            return data;
        return std::nullopt;
    }
}
