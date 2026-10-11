#include "stdafx.h"
#include "content/stfs.h"
#include "text.h"

#include <unordered_set>

namespace fs = std::filesystem;

namespace dao::content
{
    namespace
    {
        constexpr uint32_t kBlock = 0x1000;
        // Data blocks covered by one hash table at each level. The blocks under one
        // level-0 table are next to each other in the file.
        constexpr uint32_t kLevels[] = { 170, 170 * 170, 170 * 170 * 170 };
        constexpr size_t kHeaderRead = 0x1800;
        constexpr uint32_t kMaxTableBlocks = 1024; // 65,536 entries
        constexpr int kMaxDepth = 32;

        uint32_t Be32(const uint8_t* p)
        {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }

        uint32_t Le24(const uint8_t* p)
        {
            return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
        }

        bool IsMagic(const uint8_t* p)
        {
            return memcmp(p, "CON ", 4) == 0 || memcmp(p, "LIVE", 4) == 0 || memcmp(p, "PIRS", 4) == 0;
        }

        // A name Windows can hold that stays in its folder.
        bool ValidName(std::string_view name)
        {
            if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ')
                return false;
            for (const char c : name)
            {
                if (uint8_t(c) < 32 || strchr("<>:\"/\\|?*", c))
                    return false;
            }
            std::string stem(name.substr(0, name.find('.')));
            for (char& c : stem)
                c = char(toupper(uint8_t(c)));
            if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
                return false;
            // COM and LPT with a digit, or with a superscript 1-3 (in UTF-8, as FromUtf8 reads it).
            if (stem.starts_with("COM") || stem.starts_with("LPT"))
            {
                const std::string_view number = std::string_view(stem).substr(3);
                if ((number.size() == 1 && isdigit(uint8_t(number[0]))) || number == "\xC2\xB9" ||
                    number == "\xC2\xB2" || number == "\xC2\xB3")
                    return false;
            }
            return true;
        }
    }

    StfsPackage::~StfsPackage()
    {
        if (m_file != INVALID_HANDLE_VALUE)
            CloseHandle(m_file);
    }

    bool StfsPackage::IsPackage(const fs::path& path)
    {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        uint8_t magic[4] = {};
        DWORD read = 0;
        const bool ok = ReadFile(file, magic, sizeof(magic), &read, nullptr) && read == sizeof(magic) && IsMagic(magic);
        CloseHandle(file);
        return ok;
    }

    std::unique_ptr<StfsPackage> StfsPackage::Open(const fs::path& path, std::string& error)
    {
        std::unique_ptr<StfsPackage> package(new StfsPackage());
        package->m_file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER size{};
        if (package->m_file == INVALID_HANDLE_VALUE || !GetFileSizeEx(package->m_file, &size))
        {
            error = "can't be read";
            return nullptr;
        }
        package->m_size = uint64_t(size.QuadPart);
        if (!package->ReadHeader(error) || !package->ReadTable(error))
            return nullptr;
        return package;
    }

    bool StfsPackage::ReadAt(uint64_t offset, void* buffer, size_t size)
    {
        if (offset + size > m_size)
            return false;
        OVERLAPPED at{};
        at.Offset = DWORD(offset);
        at.OffsetHigh = DWORD(offset >> 32);
        DWORD read = 0;
        return ReadFile(m_file, buffer, DWORD(size), &read, &at) && read == size;
    }

    // The header's fields are at the offsets Xenia's stfs_container_device.cc and
    // Free60's STFS page give.
    bool StfsPackage::ReadHeader(std::string& error)
    {
        std::vector<uint8_t> h(kHeaderRead);
        if (!ReadAt(0, h.data(), h.size()) || !IsMagic(h.data()))
        {
            error = "not an Xbox 360 package";
            return false;
        }
        const uint32_t headerSize = Be32(&h[0x340]);
        contentType = Be32(&h[0x344]);
        mediaId = Be32(&h[0x354]);
        titleId = Be32(&h[0x360]);
        static constexpr char kHex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < 20; i++)
        {
            contentId += kHex[h[0x32C + i] >> 4];
            contentId += kHex[h[0x32C + i] & 0xF];
        }
        const uint8_t* vd = &h[0x379];
        const bool readOnly = vd[2] & 1;
        m_tableBlocks = vd[3] | (uint32_t(vd[4]) << 8);
        m_tableBlock = Le24(vd + 5);
        m_totalBlocks = Be32(vd + 0x1C);
        const uint32_t volumeType = Be32(&h[0x3A9]);
        for (size_t i = 0; i < 0x40; i++)
        {
            const char16_t c = char16_t((h[0x411 + 2 * i] << 8) | h[0x412 + 2 * i]);
            if (!c)
                break;
            displayName += c;
        }
        if (volumeType != 0)
        {
            error = "not an STFS package (an SVOD one, as games on demand are)";
            return false;
        }
        if (!readOnly)
        {
            error = "a package in the writable format (as saves are), not downloadable content";
            return false;
        }
        m_base = (uint64_t(headerSize) + kBlock - 1) / kBlock * kBlock;
        if (headerSize < 0x1000 || headerSize > 0x10000 || m_base > m_size)
        {
            error = "damaged (bad header size)";
            return false;
        }
        if (m_tableBlocks == 0 || m_tableBlocks > kMaxTableBlocks || m_totalBlocks >= kLevels[2])
        {
            error = "damaged (bad volume descriptor)";
            return false;
        }
        return true;
    }

    // Where data block `block` is: after the hash tables in front of it.
    uint64_t StfsPackage::BlockOffset(uint32_t block) const
    {
        uint64_t physical = block;
        for (const uint32_t level : kLevels)
        {
            physical += (uint64_t(block) + level) / level;
            if (block < level)
                break;
        }
        return m_base + (physical << 12);
    }

    // `count` blocks from `block`, all under one level-0 hash table (so next to each
    // other in the file): `size` bytes, the last block maybe only partly.
    bool StfsPackage::ReadRun(uint32_t block, uint32_t count, uint8_t* buffer, size_t size, std::string& error)
    {
        if (count == 0 || uint64_t(block) + count > m_totalBlocks ||
            block / kLevels[0] != (block + count - 1) / kLevels[0])
        {
            error = "damaged (block out of range)";
            return false;
        }
        if (!ReadAt(BlockOffset(block), buffer, size))
        {
            error = "damaged (truncated)";
            return false;
        }
        return true;
    }

    bool StfsPackage::ReadBlock(uint32_t block, void* buffer, size_t size, std::string& error)
    {
        if (block >= m_totalBlocks)
        {
            error = "damaged (block out of range)";
            return false;
        }
        if (!ReadAt(BlockOffset(block), buffer, size))
        {
            error = "damaged (truncated)";
            return false;
        }
        return true;
    }

    // The next block of a chain, from the block's level-0 hash table entry.
    bool StfsPackage::NextBlock(uint32_t block, uint32_t& next, std::string& error)
    {
        uint64_t table = 0;
        if (block >= kLevels[0])
        {
            table = uint64_t(block / kLevels[0]) * (kLevels[0] + 1) + block / kLevels[1] + 1;
            if (block >= kLevels[1])
                table++;
        }
        // The whole table is read once (the last may end the file early: as much of
        // it as there is).
        const uint64_t at = m_base + (table << 12);
        if (at != m_hashTableAt)
        {
            m_hashTableAt = UINT64_MAX;
            m_hashTable.assign(kBlock, 0);
            const size_t size = size_t(std::min<uint64_t>(kBlock, m_size > at ? m_size - at : 0));
            if (size == 0 || !ReadAt(at, m_hashTable.data(), size))
            {
                error = "damaged (truncated hash table)";
                return false;
            }
            m_hashTableAt = at;
        }
        const size_t entry = (block % kLevels[0]) * 0x18 + 0x14;
        if (m_hashTableAt + entry + 4 > m_size)
        {
            error = "damaged (truncated hash table)";
            return false;
        }
        next = Be32(&m_hashTable[entry]) & 0xFFFFFF;
        return true;
    }

    bool StfsPackage::ReadTable(std::string& error)
    {
        struct Folder
        {
            std::string path;
            int depth;
        };
        std::unordered_map<uint32_t, Folder> folders{ { 0xFFFF, { "", 0 } } };
        std::unordered_set<std::string> seen;
        // Files may share blocks: their sizes together are held to the package's.
        uint64_t total = 0;
        std::vector<uint8_t> data(kBlock);
        uint32_t index = 0;
        uint32_t block = m_tableBlock;
        for (uint32_t n = 0; n < m_tableBlocks; n++)
        {
            if (!ReadBlock(block, data.data(), kBlock, error))
                return false;
            for (uint32_t j = 0; j < kBlock / 0x40; j++, index++)
            {
                const uint8_t* e = &data[j * 0x40];
                if (e[0] == 0)
                    break;
                const uint8_t flags = e[40];
                const std::string name(reinterpret_cast<const char*>(e), std::min<size_t>(flags & 0x3F, 40));
                const uint32_t parent = (uint32_t(e[50]) << 8) | e[51];
                const uint64_t length = Be32(e + 52);
                if (!ValidName(name))
                {
                    error = "has a file name Windows can't hold";
                    return false;
                }
                const auto folder = folders.find(parent);
                if (folder == folders.end())
                {
                    error = "damaged (entry before its folder)";
                    return false;
                }
                if (folder->second.depth >= kMaxDepth)
                {
                    error = "damaged (folders too deep)";
                    return false;
                }
                std::string path = folder->second.path + name;
                if (!seen.insert(Lower(path)).second)
                {
                    error = "damaged (a file listed twice)";
                    return false;
                }
                if (flags & 0x80)
                {
                    folders[index] = { path + "/", folder->second.depth + 1 };
                }
                else
                {
                    total += length;
                    if (total > uint64_t(m_totalBlocks) << 12)
                    {
                        error = "damaged (bad file size)";
                        return false;
                    }
                    files.push_back({ std::move(path), length, Le24(e + 47), (flags & 0x40) != 0 });
                }
            }
            if (!NextBlock(block, block, error))
                return false;
        }
        return true;
    }

    bool StfsPackage::Extract(const fs::path& into, std::stop_token stop, std::string& error)
    {
        std::error_code ec;
        // Up to a level-0 table's blocks at a time, where a file's blocks are in order.
        std::vector<uint8_t> buffer(size_t(kLevels[0]) * kBlock);
        for (const StfsFile& file : files)
        {
            if (stop.stop_requested())
            {
                error = "stopped";
                return false;
            }
            const fs::path target = into / FromUtf8(file.path);
            fs::create_directories(target.parent_path(), ec);
            const HANDLE out = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (out == INVALID_HANDLE_VALUE)
            {
                error = fmt::format("can't write {}", file.path);
                return false;
            }
            uint32_t block = file.startBlock;
            uint64_t left = file.size;
            bool ok = true;
            while (ok && left > 0)
            {
                uint32_t count = 1;
                if (file.contiguous)
                {
                    const uint64_t blocks = (left + kBlock - 1) / kBlock;
                    count = uint32_t(std::min<uint64_t>(blocks, kLevels[0] - block % kLevels[0]));
                }
                const DWORD n = DWORD(std::min<uint64_t>(uint64_t(count) * kBlock, left));
                DWORD written = 0;
                ok = ReadRun(block, count, buffer.data(), n, error) &&
                     WriteFile(out, buffer.data(), n, &written, nullptr) && written == n;
                left -= n;
                if (ok && left > 0)
                {
                    if (file.contiguous)
                        block += count;
                    else
                        ok = NextBlock(block, block, error);
                }
            }
            CloseHandle(out);
            if (!ok)
            {
                if (error.empty())
                    error = fmt::format("can't write {}", file.path);
                return false;
            }
        }
        return true;
    }
}
