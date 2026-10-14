// Builds the game's menus with our additions from the user's own game files: a
// menu movie (a Scaleform movie: Flash 8 with ActionScript 2) is read from a
// mod's override folder or the disc's guiexport.erf, a compiled script is
// inserted, and the result is written to userdata/generated and served in place
// of the override path, above the disc and the mods. The engine reads loose
// files in packages/core/override before its archives.
//   optionsmenu.gfx (the Options menu): the Graphics tab, graphics_tab.as
//   mainmenu.gfx (the START menu): the Photo Mode row, pause_menu.as
#include "stdafx.h"
#include "ui/erf.h"
#include "files.h"
#include "text.h"
#include "ui/menu_movies.h"
#include "ui/graphics_tab_as.h"
#include "ui/pause_menu_as.h"
#include "kernel/vfs.h"

namespace fs = std::filesystem;

namespace dao::ui
{
    namespace
    {
        constexpr std::string_view kOverrideFolder = "\\Device\\Cdrom0\\packages\\core\\override\\";
        constexpr const char* kArchivePath = "\\Device\\Cdrom0\\packages\\core\\data\\guiexport.erf";

        // A sprite of the movie the script attaches by name: the movie doesn't
        // export it, the patch does (if the sprite holds `label`, as expected).
        struct SpriteExport
        {
            uint16_t sprite;
            std::string_view label;
            std::string_view name;
        };

        struct MoviePatch
        {
            std::string_view movie;          // its name in guiexport.erf
            std::string_view menu;           // for the log
            std::string_view addition;       // what the patch adds, for the log
            std::span<const uint8_t> script; // a DoAction body (as2c's output)
            // Text only a patched movie holds (a mod's copy of our output is refused).
            std::string_view marker;
            std::optional<SpriteExport> sprite;
        };

        // The Graphics tab attaches the section header sprite (a bar with
        // LabelGameplay_mc: DefineSprite 387 of the disc's optionsmenu.gfx). The
        // START menu's marker is the message its row sends (pause_menu.as, handled
        // in graphics_menu.cpp).
        constexpr MoviePatch kOptionsMenu{ "optionsmenu.gfx", "Options menu", "Graphics tab", kGraphicsTabScript,
            "DAORecomp_SectionHeader", SpriteExport{ 387, "LabelGameplay_mc", "DAORecomp_SectionHeader" } };
        constexpr MoviePatch kPauseMenu{ "mainmenu.gfx", "START menu", "Photo Mode row", kPauseMenuScript,
            "DAORecomp.PhotoMode", std::nullopt };

        std::string OverridePath(std::string_view movie)
        {
            return std::string(kOverrideFolder).append(movie);
        }
        // Far more than the movie (about 330 KB unpacked); bounds what's read and
        // what it unpacks to.
        constexpr uint32_t kMaxMovieSize = 64u << 20;
        // What a movie may unpack to beyond its header's length, which is only a
        // hint (the game's unpack to a few KB more).
        constexpr uint32_t kMovieSizeSlack = 1u << 20;

        constexpr uint16_t kTagEnd = 0;
        constexpr uint16_t kTagDoAction = 12;
        constexpr uint16_t kTagDefineSprite = 39;
        constexpr uint16_t kTagExportAssets = 56;

        uint16_t Read16(std::span<const uint8_t> data, size_t at)
        {
            return uint16_t(data[at] | data[at + 1] << 8);
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

        // The movie as the engine would load it: a mod's loose copy, else the disc's.
        std::optional<std::vector<uint8_t>> ReadMovie(std::string_view movie, std::string& source)
        {
            std::error_code ec;
            if (const auto loose = kernel::VfsResolve(OverridePath(movie)); loose && fs::is_regular_file(*loose, ec))
            {
                const uint64_t size = fs::file_size(*loose, ec);
                if (ec || size > kMaxMovieSize)
                    return std::nullopt;
                source = Utf8(*loose);
                return ReadFileBytes(*loose, 0, uint32_t(size));
            }
            const auto archive = kernel::VfsResolve(kArchivePath);
            if (!archive)
                return std::nullopt;
            source = Utf8(*archive);
            return ReadErfEntry(*archive, movie, kMaxMovieSize);
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
                tag.bodySize = ReadLE32(data, at + 2);
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

        // Inserts the script after the root's first DoAction (the one that starts
        // the scene class, its packages loaded by then), with an ExportAssets for
        // the patch's sprite before it. The movie comes out uncompressed.
        std::optional<std::vector<uint8_t>> PatchMovie(
            const MoviePatch& patch, std::span<const uint8_t> file, std::string& error)
        {
            // "GFX" (uncompressed) or "CFX" (zlib after the 8-byte header).
            if (file.size() < 8 || file[1] != 'F' || file[2] != 'X' || (file[0] != 'C' && file[0] != 'G'))
            {
                error = "not a Scaleform movie";
                return std::nullopt;
            }
            const uint32_t size = ReadLE32(file, 4);
            std::vector<uint8_t> unpacked;
            std::span<const uint8_t> movie = file;
            if (file[0] == 'C')
            {
                const uint32_t hint = std::min(size - std::min<uint32_t>(size, 8), kMaxMovieSize - kMovieSizeSlack);
                auto body = Inflate(file.subspan(8), hint, kMovieSizeSlack);
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

            bool spriteFound = false;
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
                if (patch.sprite && tag->code == kTagDefineSprite && body.size() >= 2 &&
                    Read16(body, 0) == patch.sprite->sprite)
                    spriteFound = Contains(body, patch.sprite->label);
                at += tag->headerSize + tag->bodySize;
                if (tag->code == kTagDoAction)
                    insertAt = at;
            }
            if (Contains(movie, patch.marker))
            {
                error = "already patched";
                return std::nullopt;
            }

            std::vector<uint8_t> out(movie.begin(), movie.begin() + *insertAt);
            out[0] = 'G';
            if (spriteFound)
            {
                std::vector<uint8_t> exports;
                Append16(exports, 1);
                Append16(exports, patch.sprite->sprite);
                exports.insert(exports.end(), patch.sprite->name.begin(), patch.sprite->name.end());
                exports.push_back(0);
                AppendTag(out, kTagExportAssets, exports);
            }
            else if (patch.sprite)
            {
                LOGW("{}: no sprite {} to export as {}", patch.menu, patch.sprite->sprite, patch.sprite->name);
            }
            AppendTag(out, kTagDoAction, patch.script);
            out.insert(out.end(), movie.begin() + *insertAt, movie.end());
            const uint32_t length = uint32_t(out.size());
            for (int i = 0; i < 4; ++i)
                out[4 + i] = uint8_t(length >> (8 * i));
            return out;
        }

        // Writes the patched movie to userdata/generated and serves it at the
        // movie's override path. False (the menu stays the game's) if it can't
        // be patched.
        bool InstallMovie(const MoviePatch& patch)
        {
            std::string source;
            const auto movie = ReadMovie(patch.movie, source);
            if (!movie)
            {
                if (source.empty())
                    LOGW("{}: {} not found; no {}", patch.menu, patch.movie, patch.addition);
                else
                    LOGW("{}: can't read {} from {}; no {}", patch.menu, patch.movie, source, patch.addition);
                return false;
            }
            std::string error;
            const auto patched = PatchMovie(patch, *movie, error);
            if (!patched)
            {
                LOGW("{}: can't patch {} from {} ({}); no {}", patch.menu, patch.movie, source, error,
                    patch.addition);
                return false;
            }

            // Rewritten only when it changes (a new game version, mod or script).
            const fs::path path =
                kernel::VfsUserRoot() / "generated" / "packages" / "core" / "override" / FromUtf8(patch.movie);
            std::error_code ec;
            const uint64_t existingSize = fs::file_size(path, ec);
            const auto existing = !ec && existingSize == patched->size()
                                      ? ReadFileBytes(path, 0, uint32_t(existingSize))
                                      : std::nullopt;
            if (!existing || *existing != *patched)
            {
                fs::create_directories(path.parent_path(), ec);
                if (!WriteFileAtomically(path,
                        std::string_view(reinterpret_cast<const char*>(patched->data()), patched->size())))
                {
                    LOGW("{}: can't write {}; no {}", patch.menu, Utf8(path), patch.addition);
                    return false;
                }
            }
            kernel::VfsAddFile(OverridePath(patch.movie), path);
            LOGI("{}: {} added ({} from {})", patch.menu, patch.addition, patch.movie, source);
            return true;
        }
    }

    bool InstallOptionsMovie()
    {
        return InstallMovie(kOptionsMenu);
    }

    bool InstallPauseMenuMovie()
    {
        return InstallMovie(kPauseMenu);
    }
}
