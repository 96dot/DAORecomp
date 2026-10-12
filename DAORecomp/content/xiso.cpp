#include "stdafx.h"
#include "content/xiso.h"

#include <fstream>
#include <ranges>

#include "files.h"
#include "text.h"

namespace fs = std::filesystem;

namespace dao::content
{
    namespace
    {
        constexpr uint64_t kSector = 2048;
        constexpr std::string_view kMagic = "MICROSOFT*XBOX*MEDIA";
        // Where the game partition starts: plain XDVDFS, XGD1, XGD2, XGD3.
        constexpr uint64_t kPartitionOffsets[] = { 0x0, 0x18300000, 0xFD90000, 0x2080000 };
        // A directory's table is a few KB; anything far larger isn't one.
        constexpr uint32_t kMaxTableSize = 16 << 20;
        constexpr uint8_t kDirectory = 0x10;
        // An entry: left and right subtree (in 4-byte units), sector, size, attributes,
        // name length, then the name.
        constexpr size_t kEntryHeader = 14;

        struct Entry
        {
            uint32_t sector = 0, size = 0;
            bool directory = false;
        };

        template <typename T>
        T ReadLe(const uint8_t* p)
        {
            T value;
            memcpy(&value, p, sizeof(value));
            return value;
        }

        bool ReadAt(std::ifstream& in, uint64_t offset, void* data, size_t size)
        {
            in.clear();
            return in.seekg(std::streamoff(offset)) && in.read(static_cast<char*>(data), std::streamsize(size));
        }

        // Finds `name` (any case) in the directory table at `dir`, visiting its binary
        // tree once per entry.
        std::optional<Entry> Find(std::ifstream& in, uint64_t base, const Entry& dir, std::string_view name)
        {
            if (!dir.directory || dir.size == 0 || dir.size > kMaxTableSize)
                return std::nullopt;
            std::vector<uint8_t> table(dir.size);
            if (!ReadAt(in, base + dir.sector * kSector, table.data(), table.size()))
                return std::nullopt;
            std::vector<bool> seen(table.size() / 4 + 1);
            std::vector<uint32_t> stack = { 0 };
            const std::string wanted = Lower(name);
            while (!stack.empty())
            {
                const uint32_t offset = stack.back();
                stack.pop_back();
                if (offset + kEntryHeader > table.size() || seen[offset / 4])
                    continue;
                seen[offset / 4] = true;
                const uint8_t* p = table.data() + offset;
                const uint16_t left = ReadLe<uint16_t>(p), right = ReadLe<uint16_t>(p + 2);
                const uint32_t sector = ReadLe<uint32_t>(p + 4);
                if (left == 0xFFFF && right == 0xFFFF && sector == 0xFFFFFFFF)
                    continue;
                const size_t nameLength = p[13];
                if (offset + kEntryHeader + nameLength > table.size())
                    continue;
                const std::string_view entryName(reinterpret_cast<const char*>(p + kEntryHeader), nameLength);
                if (Lower(entryName) == wanted)
                    return Entry{ sector, ReadLe<uint32_t>(p + 8), (p[12] & kDirectory) != 0 };
                if (left && left != 0xFFFF)
                    stack.push_back(uint32_t(left) * 4);
                if (right && right != 0xFFFF)
                    stack.push_back(uint32_t(right) * 4);
            }
            return std::nullopt;
        }
    }

    bool ExtractIsoFile(const fs::path& iso, std::string_view path, const fs::path& out, uint64_t maxSize,
        std::string& error)
    {
        std::ifstream in(iso, std::ios::binary);
        if (!in)
        {
            error = "can't open the disc image";
            return false;
        }
        std::optional<uint64_t> base;
        for (const uint64_t offset : kPartitionOffsets)
        {
            char magic[kMagic.size()];
            if (ReadAt(in, offset + 32 * kSector, magic, sizeof(magic)) &&
                std::string_view(magic, sizeof(magic)) == kMagic)
            {
                base = offset;
                break;
            }
        }
        uint8_t root[8];
        if (!base || !ReadAt(in, *base + 32 * kSector + kMagic.size(), root, sizeof(root)))
        {
            error = "not an Xbox 360 disc image";
            return false;
        }

        std::optional<Entry> entry = Entry{ ReadLe<uint32_t>(root), ReadLe<uint32_t>(root + 4), true };
        for (const auto part : std::views::split(path, '/'))
        {
            entry = Find(in, *base, *entry, std::string_view(part.begin(), part.end()));
            if (!entry)
            {
                error = fmt::format("{} isn't on the disc image", path);
                return false;
            }
        }
        if (entry->directory || entry->size > maxSize)
        {
            error = fmt::format("{} on the disc image isn't a file it can use", path);
            return false;
        }

        std::vector<char> data(entry->size);
        if (!ReadAt(in, *base + entry->sector * kSector, data.data(), data.size()) ||
            !WriteFileAtomically(out, std::string_view(data.data(), data.size())))
        {
            error = fmt::format("can't copy {} out of the disc image", path);
            return false;
        }
        return true;
    }
}
