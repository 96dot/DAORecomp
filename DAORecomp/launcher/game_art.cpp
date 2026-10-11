#include "stdafx.h"
#include "launcher/game_art.h"
#include "ui/erf.h"

namespace fs = std::filesystem;

namespace dao::launcher
{
    namespace
    {
        constexpr const char* kArchive = "packages/core/data/guiexport.erf";
        // The atlases are at most 2048x2048 DXT5 (4 MB) and their descriptions far
        // smaller; bounds what's read.
        constexpr uint32_t kMaxEntrySize = 32u << 20;
        constexpr int kMaxAtlasSize = 4096;

        // The texture's description at the end of an .xds: the format (low 6 bits;
        // 0x12 DXT1, 0x14 DXT4_5) and the size (width - 1 and height - 1 in 13 bits
        // each), big-endian.
        constexpr size_t kTrailerSize = 52;
        constexpr size_t kTrailerFormat = 0x20;
        constexpr size_t kTrailerSize2D = 0x24;
        constexpr uint32_t kFormatDxt1 = 0x12;
        constexpr uint32_t kFormatDxt5 = 0x14;

        struct Sprite
        {
            int atlas;
            int x, y, width, height;
        };

        struct Atlas
        {
            int width = 0, height = 0;
            bool dxt5 = false;
            std::vector<uint8_t> blocks; // tiled, as stored
        };

        uint32_t ReadBig32(std::span<const uint8_t> data, size_t at)
        {
            return uint32_t(data[at]) << 24 | data[at + 1] << 16 | data[at + 2] << 8 | data[at + 3];
        }

        // The value of attribute `name` in `tag`, if any.
        std::optional<std::string_view> Attribute(std::string_view tag, std::string_view name)
        {
            const std::string key = " " + std::string(name) + "=\"";
            const size_t at = tag.find(key);
            if (at == std::string_view::npos)
                return std::nullopt;
            const size_t start = at + key.size(), end = tag.find('"', start);
            if (end == std::string_view::npos)
                return std::nullopt;
            return tag.substr(start, end - start);
        }

        std::vector<double> Numbers(std::string_view text)
        {
            std::vector<double> out;
            const std::string copy(text);
            const char* p = copy.c_str();
            for (char* end = nullptr;; p = end)
            {
                const double value = strtod(p, &end);
                if (end == p)
                    break;
                out.push_back(value);
            }
            return out;
        }

        // An atlas description (<base>_dat.xml): AtlasTexture "<base>_NN.dds" elements
        // holding SourceTexture elements, each with its name, place (OffsetAndScale:
        // x, y, width, height as fractions of AtlasSize) and OriginalImageSize.
        void ReadDescription(std::string_view xml, std::unordered_map<std::string, Sprite>& sprites,
            std::vector<std::string>& atlasNames)
        {
            int atlas = -1;
            for (size_t at = xml.find('<'); at != std::string_view::npos; at = xml.find('<', at + 1))
            {
                const size_t end = xml.find('>', at);
                if (end == std::string_view::npos)
                    break;
                const std::string_view tag = xml.substr(at, end - at);
                if (tag.starts_with("<AtlasTexture"))
                {
                    const auto name = Attribute(tag, "Name");
                    if (!name || !name->ends_with(".dds"))
                        continue;
                    atlas = int(atlasNames.size());
                    atlasNames.emplace_back(name->substr(0, name->size() - 4));
                }
                else if (tag.starts_with("<SourceTexture") && atlas >= 0)
                {
                    const auto name = Attribute(tag, "Name");
                    const auto place = Attribute(tag, "OffsetAndScale");
                    const auto atlasSize = Attribute(tag, "AtlasSize");
                    const auto size = Attribute(tag, "OriginalImageSize");
                    if (!name || !place || !atlasSize || !size)
                        continue;
                    const auto p = Numbers(*place), a = Numbers(*atlasSize), s = Numbers(*size);
                    if (p.size() != 4 || a.size() != 2 || s.size() != 2)
                        continue;
                    std::string key(*name);
                    if (key.ends_with(".dds"))
                        key.resize(key.size() - 4);
                    sprites[key] = Sprite{ atlas, int(std::lround(p[0] * a[0])), int(std::lround(p[1] * a[1])),
                        int(s[0]), int(s[1]) };
                }
            }
        }

        std::optional<Atlas> ReadAtlas(const fs::path& archive, const std::string& name)
        {
            auto data = ui::ReadErfEntry(archive, name + ".xds", kMaxEntrySize);
            if (!data || data->size() < kTrailerSize)
                return std::nullopt;
            const std::span<const uint8_t> trailer(data->data() + data->size() - kTrailerSize, kTrailerSize);
            const uint32_t format = ReadBig32(trailer, kTrailerFormat) & 0x3F;
            const uint32_t size = ReadBig32(trailer, kTrailerSize2D);
            Atlas atlas;
            atlas.width = int(size & 0x1FFF) + 1;
            atlas.height = int((size >> 13) & 0x1FFF) + 1;
            atlas.dxt5 = format == kFormatDxt5;
            if ((format != kFormatDxt1 && format != kFormatDxt5) || atlas.width > kMaxAtlasSize ||
                atlas.height > kMaxAtlasSize || atlas.width % 4 || atlas.height % 4)
                return std::nullopt;
            const size_t blocksSize = size_t(atlas.width / 4) * (atlas.height / 4) * (atlas.dxt5 ? 16 : 8);
            if (data->size() < blocksSize + kTrailerSize)
                return std::nullopt;
            data->resize(blocksSize);
            atlas.blocks = std::move(*data);
            return atlas;
        }

        // The index of block (x, y) of a tiled texture `width` blocks wide whose
        // blocks are 2^logBytes bytes (Xenos 2D tiling, as XGAddress2DTiledOffset).
        uint32_t TiledBlock(uint32_t x, uint32_t y, uint32_t width, uint32_t logBytes)
        {
            const uint32_t alignedWidth = (width + 31) & ~31u;
            const uint32_t macro = ((x >> 5) + (y >> 5) * (alignedWidth >> 5)) << (logBytes + 7);
            const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << logBytes;
            const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + logBytes)) +
                ((y & 1) << 4);
            return (((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
                       (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> logBytes;
        }

        // The 16 RGBA texels of a DXT1 colour block (`opaque` for DXT5's, which has no
        // transparent texels).
        void DecodeColour(const uint8_t* block, bool opaque, uint8_t (*out)[4])
        {
            const uint32_t c0 = block[0] | block[1] << 8, c1 = block[2] | block[3] << 8;
            int palette[4][4];
            for (int i = 0; i < 2; i++)
            {
                const uint32_t c = i ? c1 : c0;
                palette[i][0] = int(c >> 11) * 255 / 31;
                palette[i][1] = int((c >> 5) & 63) * 255 / 63;
                palette[i][2] = int(c & 31) * 255 / 31;
                palette[i][3] = 255;
            }
            const bool four = opaque || c0 > c1;
            for (int ch = 0; ch < 3; ch++)
            {
                palette[2][ch] = four ? (2 * palette[0][ch] + palette[1][ch]) / 3 : (palette[0][ch] + palette[1][ch]) / 2;
                palette[3][ch] = four ? (palette[0][ch] + 2 * palette[1][ch]) / 3 : 0;
            }
            palette[2][3] = 255;
            palette[3][3] = four ? 255 : 0;
            uint32_t bits = block[4] | block[5] << 8 | block[6] << 16 | uint32_t(block[7]) << 24;
            for (int i = 0; i < 16; i++, bits >>= 2)
            {
                for (int ch = 0; ch < 4; ch++)
                    out[i][ch] = uint8_t(palette[bits & 3][ch]);
            }
        }

        // DXT5's alpha block over the texels' alpha.
        void DecodeAlpha(const uint8_t* block, uint8_t (*out)[4])
        {
            const int a0 = block[0], a1 = block[1];
            int palette[8] = { a0, a1 };
            for (int i = 2; i < 8; i++)
            {
                if (a0 > a1)
                    palette[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
                else
                    palette[i] = i < 6 ? ((6 - i) * a0 + (i - 1) * a1) / 5 : (i == 6 ? 0 : 255);
            }
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++)
                bits |= uint64_t(block[2 + i]) << (8 * i);
            for (int i = 0; i < 16; i++, bits >>= 3)
                out[i][3] = uint8_t(palette[bits & 7]);
        }

        ArtImage DecodeSprite(const Atlas& atlas, const Sprite& sprite)
        {
            ArtImage image;
            image.width = sprite.width;
            image.height = sprite.height;
            image.rgba.resize(size_t(sprite.width) * sprite.height * 4);
            const uint32_t blockSize = atlas.dxt5 ? 16 : 8, logBytes = atlas.dxt5 ? 4 : 3;
            const uint32_t widthBlocks = uint32_t(atlas.width / 4);
            for (int by = sprite.y / 4; by <= (sprite.y + sprite.height - 1) / 4; by++)
            {
                for (int bx = sprite.x / 4; bx <= (sprite.x + sprite.width - 1) / 4; bx++)
                {
                    // Stored as big-endian 16-bit words.
                    uint8_t block[16];
                    const uint8_t* src = &atlas.blocks[size_t(TiledBlock(bx, by, widthBlocks, logBytes)) * blockSize];
                    for (uint32_t i = 0; i < blockSize; i += 2)
                    {
                        block[i] = src[i + 1];
                        block[i + 1] = src[i];
                    }
                    uint8_t texels[16][4];
                    if (atlas.dxt5)
                    {
                        DecodeColour(block + 8, true, texels);
                        DecodeAlpha(block, texels);
                    }
                    else
                    {
                        DecodeColour(block, false, texels);
                    }
                    for (int i = 0; i < 16; i++)
                    {
                        const int x = bx * 4 + i % 4 - sprite.x, y = by * 4 + i / 4 - sprite.y;
                        if (x >= 0 && y >= 0 && x < sprite.width && y < sprite.height)
                            memcpy(&image.rgba[(size_t(y) * sprite.width + x) * 4], texels[i], 4);
                    }
                }
            }
            return image;
        }
    }

    std::unordered_map<std::string, ArtImage> LoadGameArt(const fs::path& gameRoot,
        std::span<const std::string_view> atlases, std::span<const std::string_view> names)
    {
        std::unordered_map<std::string, ArtImage> out;
        const fs::path archive = gameRoot / kArchive;
        std::unordered_map<std::string, Sprite> sprites;
        std::vector<std::string> atlasNames;
        for (std::string_view base : atlases)
        {
            if (const auto xml = ui::ReadErfEntry(archive, std::string(base) + "_dat.xml", kMaxEntrySize))
            {
                const std::string_view text(reinterpret_cast<const char*>(xml->data()), xml->size());
                ReadDescription(text, sprites, atlasNames);
            }
        }

        std::unordered_map<int, std::optional<Atlas>> loaded;
        for (std::string_view name : names)
        {
            const auto it = sprites.find(std::string(name));
            if (it == sprites.end())
                continue;
            const Sprite& sprite = it->second;
            auto& atlas = loaded[sprite.atlas];
            if (!atlas)
            {
                atlas = ReadAtlas(archive, atlasNames[sprite.atlas]);
                if (!atlas)
                {
                    LOGW("Launcher: can't read {}", atlasNames[sprite.atlas]);
                    atlas = Atlas{};
                }
            }
            if (atlas->blocks.empty() || sprite.width <= 0 || sprite.height <= 0 || sprite.x < 0 || sprite.y < 0 ||
                sprite.x + sprite.width > atlas->width || sprite.y + sprite.height > atlas->height)
                continue;
            out.emplace(std::string(name), DecodeSprite(*atlas, sprite));
        }
        return out;
    }
}
