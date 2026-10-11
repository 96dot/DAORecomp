#include "stdafx.h"
#include "launcher/game_art.h"
#include "ui/erf.h"

#include <charconv>

namespace fs = std::filesystem;

namespace dao::launcher
{
    namespace
    {
        constexpr const char* kArchive = "packages/core/data/guiexport.erf";
        // The game's atlases are 2048 pixels wide and at most as high; this allows
        // 4096 square.
        constexpr int kMaxAtlasSize = 4096;
        // An atlas description: the game's are a few KB.
        constexpr uint32_t kMaxDescriptionSize = 1u << 20;

        // An .xds is the texture's blocks, then a 52-byte description of it: the
        // format (low 6 bits) and the size (width - 1 and height - 1 in 13 bits each),
        // big-endian.
        constexpr size_t kTrailerSize = 52;
        constexpr size_t kTrailerFormatOffset = 0x20;
        constexpr size_t kTrailerSizeOffset = 0x24;
        constexpr uint32_t kFormatDxt1 = 0x12;
        constexpr uint32_t kFormatDxt5 = 0x14;
        // The blocks are stored in 32x32-block tiles.
        constexpr int kTileBlocks = 32;
        constexpr uint32_t kMaxAtlasEntrySize =
            uint32_t(kMaxAtlasSize / 4) * uint32_t(kMaxAtlasSize / 4) * 16 + kTrailerSize;

        struct Sprite
        {
            int atlas;
            int x, y, width, height;
        };

        struct Atlas
        {
            int width = 0, height = 0;
            bool dxt5 = false;
            // As stored, in 32x32-block tiles, but byte-swapped to little-endian.
            std::vector<uint8_t> blocks;
        };

        // The value of attribute `name` in `tag` (written ` name="value"`), if any.
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

        // The numbers in `text`, separated by spaces.
        std::vector<double> Numbers(std::string_view text)
        {
            std::vector<double> out;
            const char* p = text.data();
            const char* const end = p + text.size();
            for (;;)
            {
                while (p < end && *p == ' ')
                    p++;
                double value;
                const auto [next, ec] = std::from_chars(p, end, value);
                if (ec != std::errc() || !std::isfinite(value))
                    break;
                out.push_back(value);
                p = next;
            }
            return out;
        }

        // A pixel coordinate or size in an atlas, if it can be one.
        std::optional<int> Pixels(double value)
        {
            const double rounded = std::round(value);
            if (!(rounded >= 0.0 && rounded <= kMaxAtlasSize))
                return std::nullopt;
            return int(rounded);
        }

        // An atlas description (<base>_dat.xml): AtlasTexture "<base>_NN.dds" elements
        // holding SourceTexture elements, each with its name, place (OffsetAndScale:
        // x, y, width, height as fractions of AtlasSize) and OriginalImageSize. A
        // sprite in more than one atlas is taken from the last.
        void ReadDescription(std::string_view xml, std::unordered_map<std::string, Sprite>& sprites,
            std::vector<std::string>& atlasNames)
        {
            int atlas = -1;
            for (size_t at = xml.find('<'); at != std::string_view::npos; at = xml.find('<', at))
            {
                const size_t end = xml.find('>', at);
                if (end == std::string_view::npos)
                    break;
                const std::string_view tag = xml.substr(at, end - at);
                at = end;
                if (tag.starts_with("<AtlasTexture"))
                {
                    // Its texture is the entry "<base>_NN.xds".
                    const auto name = Attribute(tag, "Name");
                    atlas = -1;
                    if (!name || !name->ends_with(".dds") || name->size() > ui::ErfArchive::kNameSize)
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
                    const auto x = Pixels(p[0] * a[0]), y = Pixels(p[1] * a[1]);
                    const auto width = Pixels(s[0]), height = Pixels(s[1]);
                    if (!x || !y || !width || !height)
                        continue;
                    std::string key(*name);
                    if (key.ends_with(".dds"))
                        key.resize(key.size() - 4);
                    sprites[key] = Sprite{ atlas, *x, *y, *width, *height };
                }
            }
        }

        std::optional<Atlas> ReadAtlas(const ui::ErfArchive& archive, const std::string& name)
        {
            const auto data = archive.Read(name + ".xds", kMaxAtlasEntrySize);
            if (!data || data->size() < kTrailerSize)
                return std::nullopt;
            const std::span<const uint8_t> trailer(data->data() + data->size() - kTrailerSize, kTrailerSize);
            const uint32_t format = ui::ReadBE32(trailer, kTrailerFormatOffset) & 0x3F;
            const uint32_t size = ui::ReadBE32(trailer, kTrailerSizeOffset);
            if (format != kFormatDxt1 && format != kFormatDxt5)
                return std::nullopt;
            Atlas atlas;
            atlas.width = int(size & 0x1FFF) + 1;
            atlas.height = int((size >> 13) & 0x1FFF) + 1;
            atlas.dxt5 = format == kFormatDxt5;
            if (atlas.width > kMaxAtlasSize || atlas.height > kMaxAtlasSize || atlas.width % 4 || atlas.height % 4)
                return std::nullopt;
            // Whole tiles, the last ones padded.
            auto tiledBlocks = [](int pixels) {
                return size_t((pixels / 4 + kTileBlocks - 1) / kTileBlocks * kTileBlocks);
            };
            const size_t blocksSize = tiledBlocks(atlas.width) * tiledBlocks(atlas.height) * (atlas.dxt5 ? 16 : 8);
            if (data->size() < blocksSize + kTrailerSize)
                return std::nullopt;
            // Stored as big-endian 16-bit words.
            atlas.blocks.resize(blocksSize);
            for (size_t i = 0; i < blocksSize; i += 2)
            {
                atlas.blocks[i] = (*data)[i + 1];
                atlas.blocks[i + 1] = (*data)[i];
            }
            return atlas;
        }

        // The index of block (x, y) of a tiled texture `width` blocks wide whose
        // blocks are 2^logBytes bytes: XGAddress2DTiledOffset of the Xbox 360 SDK (as
        // in Xenia's texture_address.cc).
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
            const bool four = opaque || c0 > c1;
            int palette[4][4];
            for (int i = 0; i < 2; i++)
            {
                const uint32_t c = i ? c1 : c0;
                palette[i][0] = int(c >> 11) * 255 / 31;
                palette[i][1] = int((c >> 5) & 63) * 255 / 63;
                palette[i][2] = int(c & 31) * 255 / 31;
            }
            for (int ch = 0; ch < 3; ch++)
            {
                const int p0 = palette[0][ch], p1 = palette[1][ch];
                palette[2][ch] = four ? (2 * p0 + p1) / 3 : (p0 + p1) / 2;
                palette[3][ch] = four ? (p0 + 2 * p1) / 3 : 0;
            }
            palette[0][3] = palette[1][3] = palette[2][3] = 255;
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
            if (a0 > a1)
            {
                for (int i = 2; i < 8; i++)
                    palette[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
            }
            else
            {
                for (int i = 2; i < 6; i++)
                    palette[i] = ((6 - i) * a0 + (i - 1) * a1) / 5;
                palette[6] = 0;
                palette[7] = 255;
            }
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++)
                bits |= uint64_t(block[2 + i]) << (8 * i);
            for (int i = 0; i < 16; i++, bits >>= 3)
                out[i][3] = uint8_t(palette[bits & 7]);
        }

        // A sprite inside the atlas.
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
                    const size_t at = size_t(TiledBlock(bx, by, widthBlocks, logBytes)) * blockSize;
                    if (at + blockSize > atlas.blocks.size())
                        continue;
                    const uint8_t* block = &atlas.blocks[at];
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

    void LoadGameArt(const fs::path& gameRoot, std::span<const std::string_view> atlases,
        std::span<const std::string_view> names, const ArtSink& sink)
    {
        const auto archive = ui::ErfArchive::Open(gameRoot / kArchive);
        if (!archive)
            return;
        std::unordered_map<std::string, Sprite> sprites;
        std::vector<std::string> atlasNames;
        for (std::string_view base : atlases)
        {
            if (const auto xml = archive->Read(std::string(base) + "_dat.xml", kMaxDescriptionSize))
            {
                const std::string_view text(reinterpret_cast<const char*>(xml->data()), xml->size());
                ReadDescription(text, sprites, atlasNames);
            }
        }

        // An atlas at a time, freed when its sprites are decoded.
        std::vector<std::pair<const Sprite*, std::string>> wanted;
        for (std::string_view name : names)
        {
            if (const auto it = sprites.find(std::string(name)); it != sprites.end())
                wanted.emplace_back(&it->second, name);
        }
        std::ranges::stable_sort(wanted, {}, [](const auto& entry) { return entry.first->atlas; });
        for (size_t i = 0; i < wanted.size();)
        {
            const int index = wanted[i].first->atlas;
            const auto atlas = ReadAtlas(*archive, atlasNames[index]);
            if (!atlas)
                LOGW("Launcher: can't read {}", atlasNames[index]);
            for (; i < wanted.size() && wanted[i].first->atlas == index; i++)
            {
                // Its pixels are in [0, kMaxAtlasSize], so the sums can't overflow.
                const Sprite& sprite = *wanted[i].first;
                if (!atlas || sprite.width == 0 || sprite.height == 0 || sprite.x + sprite.width > atlas->width ||
                    sprite.y + sprite.height > atlas->height)
                    continue;
                ArtImage image = DecodeSprite(*atlas, sprite);
                sink(wanted[i].second, image);
            }
        }
    }
}
