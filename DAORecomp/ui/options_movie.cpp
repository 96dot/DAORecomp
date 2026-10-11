// Builds the Options menu with the Graphics tab from the user's own game files:
// optionsmenu.gfx (a Scaleform movie: Flash 8 with ActionScript 2) is read from
// a mod's override folder or the disc's guiexport.erf, the compiled script of
// graphics_tab.as is inserted, and the result is written to userdata/generated,
// an overlay above the disc and the mods. The engine reads loose files in
// packages/core/override before its archives.
#include "stdafx.h"
#include "ui/graphics_menu.h"
#include "ui/graphics_tab_as.h"
#include "kernel/vfs.h"

#include <fstream>

#include "third_party/stb/stb_image.h"

namespace fs = std::filesystem;

namespace dao::ui
{
    namespace
    {
        constexpr const char* kMovieName = "optionsmenu.gfx";
        constexpr const char* kOverridePath = "\\Device\\Cdrom0\\packages\\core\\override\\optionsmenu.gfx";
        constexpr const char* kArchivePath = "\\Device\\Cdrom0\\packages\\core\\data\\guiexport.erf";
        // The section header sprite (a bar with LabelGameplay_mc) the script
        // attaches; the movie doesn't export it, the patch does.
        constexpr uint16_t kHeaderSprite = 387;
        constexpr std::string_view kHeaderLabel = "LabelGameplay_mc";
        constexpr std::string_view kHeaderExport = "DAORecomp_SectionHeader";
        // Far more than the movie (about 2 MB unpacked); bounds what's read.
        constexpr uint32_t kMaxMovieSize = 64u << 20;

        constexpr uint16_t kTagEnd = 0;
        constexpr uint16_t kTagDoAction = 12;
        constexpr uint16_t kTagDefineSprite = 39;
        constexpr uint16_t kTagExportAssets = 56;

        uint16_t Read16(std::span<const uint8_t> data, size_t at)
        {
            return uint16_t(data[at] | data[at + 1] << 8);
        }

        uint32_t Read32(std::span<const uint8_t> data, size_t at)
        {
            return data[at] | data[at + 1] << 8 | data[at + 2] << 16 | uint32_t(data[at + 3]) << 24;
        }

        void Append16(std::vector<uint8_t>& out, uint16_t value)
        {
            out.push_back(uint8_t(value));
            out.push_back(uint8_t(value >> 8));
        }

        void Append32(std::vector<uint8_t>& out, uint32_t value)
        {
            Append16(out, uint16_t(value));
            Append16(out, uint16_t(value >> 16));
        }

        // A tag in long form (any length).
        void AppendTag(std::vector<uint8_t>& out, uint16_t code, std::span<const uint8_t> body)
        {
            Append16(out, uint16_t(code << 6 | 0x3F));
            Append32(out, uint32_t(body.size()));
            out.insert(out.end(), body.begin(), body.end());
        }

        // zlib data of `size` bytes unpacked, or of any size up to kMaxMovieSize if
        // `size` is only a hint.
        std::optional<std::vector<uint8_t>> Inflate(std::span<const uint8_t> packed, uint32_t size, bool exact)
        {
            if (packed.size() > INT_MAX || size > kMaxMovieSize)
                return std::nullopt;
            int length = 0;
            char* data = stbi_zlib_decode_malloc_guesssize_headerflag(
                reinterpret_cast<const char*>(packed.data()), int(packed.size()), int(size), &length, 1);
            if (!data)
                return std::nullopt;
            std::vector<uint8_t> out;
            if (exact ? uint32_t(length) == size : length > 0 && uint32_t(length) <= kMaxMovieSize)
                out.assign(data, data + length);
            stbi_image_free(data);
            if (out.empty())
                return std::nullopt;
            return out;
        }

        std::optional<std::vector<uint8_t>> ReadFile(const fs::path& path, uint64_t offset, uint32_t size)
        {
            std::ifstream file(path, std::ios::binary);
            std::vector<uint8_t> data(size);
            if (!file.seekg(std::streamoff(offset)) ||
                !file.read(reinterpret_cast<char*>(data.data()), std::streamsize(size)))
                return std::nullopt;
            return data;
        }

        // ERF V2.1 (the packages' .erf archives): "ERF V2.1", the entry count at
        // 0x10, then 44-byte entries from 0x20: name[32], offset, packed size,
        // size. An entry is zlib-compressed when its sizes differ.
        std::optional<std::vector<uint8_t>> ReadArchiveEntry(const fs::path& path, std::string_view name)
        {
            constexpr size_t kEntriesOffset = 0x20;
            constexpr size_t kEntrySize = 44;
            constexpr uint32_t kMaxEntries = 1u << 16;

            std::error_code ec;
            const uint64_t fileSize = fs::file_size(path, ec);
            const auto header = ec ? std::nullopt : ReadFile(path, 0, kEntriesOffset);
            if (!header || memcmp(header->data(), "ERF V2.1", 8) != 0)
                return std::nullopt;
            const uint32_t count = Read32(*header, 0x10);
            if (count > kMaxEntries)
                return std::nullopt;
            const auto entries = ReadFile(path, kEntriesOffset, uint32_t(count * kEntrySize));
            if (!entries)
                return std::nullopt;
            for (uint32_t i = 0; i < count; ++i)
            {
                const std::span<const uint8_t> entry(entries->data() + i * kEntrySize, kEntrySize);
                const auto* entryName = reinterpret_cast<const char*>(entry.data());
                const std::string_view entryView(entryName, strnlen(entryName, 32));
                if (!std::ranges::equal(entryView, name, [](char a, char b) { return tolower(a) == tolower(b); }))
                    continue;
                const uint32_t offset = Read32(entry, 32), packed = Read32(entry, 36), size = Read32(entry, 40);
                if (packed > kMaxMovieSize || size > kMaxMovieSize || uint64_t(offset) + packed > fileSize)
                    return std::nullopt;
                auto data = ReadFile(path, offset, packed);
                if (!data || packed == size)
                    return data;
                return Inflate(*data, size, true);
            }
            return std::nullopt;
        }

        // The movie as the engine would load it: a mod's loose copy, else the disc's.
        std::optional<std::vector<uint8_t>> ReadOptionsMovie(std::string& source)
        {
            std::error_code ec;
            if (const auto loose = kernel::VfsResolve(kOverridePath); loose && fs::is_regular_file(*loose, ec))
            {
                const uint64_t size = fs::file_size(*loose, ec);
                if (ec || size > kMaxMovieSize)
                    return std::nullopt;
                source = loose->string();
                return ReadFile(*loose, 0, uint32_t(size));
            }
            const auto archive = kernel::VfsResolve(kArchivePath);
            if (!archive)
                return std::nullopt;
            source = archive->string();
            return ReadArchiveEntry(*archive, kMovieName);
        }

        // The tag at `at`: code, header size and body size, if it fits in `data`.
        struct Tag
        {
            uint16_t code;
            size_t headerSize;
            size_t bodySize;
        };

        std::optional<Tag> ReadTag(std::span<const uint8_t> data, size_t at)
        {
            if (at + 2 > data.size())
                return std::nullopt;
            const uint16_t codeAndLength = Read16(data, at);
            Tag tag{ uint16_t(codeAndLength >> 6), 2, size_t(codeAndLength & 0x3F) };
            if (tag.bodySize == 0x3F)
            {
                if (at + 6 > data.size())
                    return std::nullopt;
                tag.bodySize = Read32(data, at + 2);
                tag.headerSize = 6;
            }
            if (tag.bodySize > data.size() - at - tag.headerSize)
                return std::nullopt;
            return tag;
        }

        bool Contains(std::span<const uint8_t> data, std::string_view text)
        {
            return std::search(data.begin(), data.end(), text.begin(), text.end(),
                       [](uint8_t a, char b) { return a == uint8_t(b); }) != data.end();
        }

        // Inserts the script after the root's first DoAction (the one that defines
        // the scene class OptionsMenu, its packages loaded by then), with an
        // ExportAssets for the section header sprite before it. The movie comes out
        // uncompressed.
        std::optional<std::vector<uint8_t>> PatchOptionsMovie(std::span<const uint8_t> file, std::string& error)
        {
            if (file.size() < 8 || file[1] != 'F' || file[2] != 'X' || (file[0] != 'C' && file[0] != 'G'))
            {
                error = "not a Scaleform movie";
                return std::nullopt;
            }
            // The header's length is no more than a hint: the game's compressed movies
            // unpack to a few KB more (tags included).
            const uint32_t size = Read32(file, 4);
            std::vector<uint8_t> unpacked;
            std::span<const uint8_t> movie = file;
            if (file[0] == 'C')
            {
                auto body = Inflate(file.subspan(8), size - std::min<uint32_t>(size, 8), false);
                if (!body)
                {
                    error = "can't decompress the movie";
                    return std::nullopt;
                }
                unpacked.assign(file.begin(), file.begin() + 8);
                unpacked.insert(unpacked.end(), body->begin(), body->end());
                movie = unpacked;
            }

            // Header: the frame RECT (5 bits of field size, 4 fields), frame rate and count.
            if (movie.size() < 9)
            {
                error = "truncated header";
                return std::nullopt;
            }
            const size_t rectBits = 5 + 4 * (movie[8] >> 3);
            size_t at = 8 + (rectBits + 7) / 8 + 4;

            bool headerSprite = false;
            std::optional<size_t> insertAt;
            while (!insertAt)
            {
                const auto tag = ReadTag(movie, at);
                if (!tag || tag->code == kTagEnd)
                {
                    error = "no DoAction in the root timeline";
                    return std::nullopt;
                }
                const auto body = movie.subspan(at + tag->headerSize, tag->bodySize);
                if (tag->code == kTagDefineSprite && body.size() >= 2 && Read16(body, 0) == kHeaderSprite)
                    headerSprite = Contains(body, kHeaderLabel);
                at += tag->headerSize + tag->bodySize;
                if (tag->code == kTagDoAction)
                    insertAt = at;
            }
            if (Contains(movie, kHeaderExport))
            {
                error = "already patched";
                return std::nullopt;
            }

            std::vector<uint8_t> out(movie.begin(), movie.begin() + *insertAt);
            out[0] = 'G';
            if (headerSprite)
            {
                std::vector<uint8_t> exports;
                Append16(exports, 1);
                Append16(exports, kHeaderSprite);
                exports.insert(exports.end(), kHeaderExport.begin(), kHeaderExport.end());
                exports.push_back(0);
                AppendTag(out, kTagExportAssets, exports);
            }
            else
            {
                LOGW("Options menu: no section header sprite; the Graphics tab has no headers");
            }
            AppendTag(out, kTagDoAction, kGraphicsTabScript);
            out.insert(out.end(), movie.begin() + *insertAt, movie.end());
            const uint32_t length = uint32_t(out.size());
            for (int i = 0; i < 4; ++i)
                out[4 + i] = uint8_t(length >> (8 * i));
            return out;
        }
    }

    bool InstallOptionsMovie()
    {
        std::string source;
        const auto movie = ReadOptionsMovie(source);
        if (!movie)
        {
            LOGW("Options menu: can't read {} ({}); no Graphics tab", kMovieName, source.empty() ? "not found" : source);
            return false;
        }
        std::string error;
        const auto patched = PatchOptionsMovie(*movie, error);
        if (!patched)
        {
            LOGW("Options menu: can't patch {} from {} ({}); no Graphics tab", kMovieName, source, error);
            return false;
        }

        // Rewritten only when it changes (a new game version, mod or script).
        const fs::path root = kernel::VfsUserRoot() / "generated";
        const fs::path path = root / "packages" / "core" / "override" / kMovieName;
        std::error_code ec;
        const uint64_t existingSize = fs::file_size(path, ec);
        const auto existing = !ec && existingSize == patched->size() ? ReadFile(path, 0, uint32_t(existingSize))
                                                                     : std::nullopt;
        if (!existing || *existing != *patched)
        {
            fs::create_directories(path.parent_path(), ec);
            const fs::path temp = path.string() + ".tmp";
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(patched->data()), std::streamsize(patched->size()));
                if (!file)
                {
                    LOGW("Options menu: can't write {}; no Graphics tab", temp.string());
                    return false;
                }
            }
            if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                LOGW("Options menu: can't replace {} ({}); no Graphics tab", path.string(), GetLastError());
                return false;
            }
        }
        kernel::VfsAddTopOverlay("\\Device\\Cdrom0", root);
        LOGI("Options menu: Graphics tab added ({} from {})", kMovieName, source);
        return true;
    }
}
