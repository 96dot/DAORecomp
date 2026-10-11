#include "stdafx.h"
#include "mods/convert/texture.h"

#include "ui/erf.h"

#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/texture_util.h"

namespace dao::mods::convert
{
    namespace
    {
        namespace xg = xe::gpu;
        using xg::xenos::TextureFormat;

        constexpr uint32_t kPage = 4096;
        // The largest texture the 360's GPU takes.
        constexpr uint32_t kMaxSize = 8192;

        struct Format
        {
            TextureFormat format;
            uint32_t endian;      // 1: 8-in-16, 2: 8-in-32
            uint32_t blockSize;   // 1 or 4 texels
            uint32_t blockBytes;
            uint32_t swizzle;     // the fetch constant's swizzle and number-format bits
        };

        // The formats the game's own textures use (swizzle ZYXW turns the 360's
        // ARGB words back into RGBA; XYZW for compressed ones; XXX1 for L8).
        constexpr Format kL8{ TextureFormat::k_8, 0, 1, 1, 0x1400 };
        constexpr Format kArgb4{ TextureFormat::k_4_4_4_4, 1, 1, 2, 0xC14 };
        constexpr Format kArgb8{ TextureFormat::k_8_8_8_8, 2, 1, 4, 0xC14 };
        constexpr Format kXrgb8{ TextureFormat::k_8_8_8_8, 2, 1, 4, 0x1414 };
        constexpr Format kDxt1{ TextureFormat::k_DXT1, 1, 4, 8, 0xD10 };
        constexpr Format kDxt3{ TextureFormat::k_DXT2_3, 1, 4, 16, 0xD10 };
        constexpr Format kDxt5{ TextureFormat::k_DXT4_5, 1, 4, 16, 0xD10 };
        constexpr Format kDxn{ TextureFormat::k_DXN, 1, 4, 16, 0xD10 };

        uint32_t Align(uint32_t v, uint32_t a)
        {
            return (v + a - 1) / a * a;
        }

        uint32_t Blocks(uint32_t texels, const Format& f)
        {
            return (std::max(texels, 1u) + f.blockSize - 1) / f.blockSize;
        }

        // Where each mip of a 2D tiled texture with packed mips lives (as Xenia's
        // GetGuestTextureLayout finds it, for one slice).
        class Layout
        {
        public:
            Layout(const Format& f, uint32_t width, uint32_t height, uint32_t levels)
                : f_(f), width_(width), height_(height), levels_(levels),
                  logBlockBytes_(std::countr_zero(f.blockBytes)),
                  packed_(xg::texture_util::GetPackedMipLevel(width, height)),
                  pitchTexels_(Align(Blocks(width, f), 32) * f.blockSize)
            {
                const uint32_t baseRows = packed_ != 0 ? height : std::bit_ceil(height);
                basePitch_ = Align(Blocks(pitchTexels_, f), 32);
                baseStride_ = Align(basePitch_ * Align(Blocks(baseRows, f), 32) * f.blockBytes, kPage);
                uint32_t offset = 0;
                for (uint32_t level = 1; level <= std::min(levels - 1, packed_); ++level)
                {
                    const uint32_t pitch = Align(Blocks(std::max(std::bit_ceil(width) >> level, 1u), f), 32);
                    const uint32_t rows = Align(Blocks(std::max(std::bit_ceil(height) >> level, 1u), f), 32);
                    mips_.push_back({ pitch, offset });
                    offset += Align(pitch * rows * f.blockBytes, kPage);
                }
            }

            uint32_t PitchTexels() const { return pitchTexels_; }

            // D3D leaves the mip address at 0 when the base is already the packed tail
            // (the mips are stored in it too).
            uint32_t MipAddress() const { return levels_ > 1 && packed_ != 0 ? baseStride_ : 0; }

            // The byte address of block (x, y) of a mip.
            uint32_t Address(uint32_t level, uint32_t x, uint32_t y) const
            {
                uint32_t pitch = basePitch_, offset = 0;
                if (packed_ != 0 && level != 0)
                {
                    const auto& mip = mips_[std::min(level, packed_) - 1];
                    pitch = mip.pitch;
                    offset = MipAddress() + mip.offset;
                }
                if (level >= packed_)
                {
                    uint32_t px, py, pz;
                    xg::texture_util::GetPackedMipOffset(width_, height_, 1, f_.format, level, px, py, pz);
                    x += px;
                    y += py;
                }
                return offset + uint32_t(xg::texture_address::Tiled2D(int32_t(x), int32_t(y), pitch, logBlockBytes_));
            }

        private:
            struct Mip
            {
                uint32_t pitch, offset;
            };

            const Format& f_;
            uint32_t width_, height_, levels_, logBlockBytes_, packed_, pitchTexels_;
            uint32_t basePitch_ = 0, baseStride_ = 0;
            std::vector<Mip> mips_;
        };

        std::vector<uint8_t> Encode(const Format& f, uint32_t width, uint32_t height,
            const std::vector<std::vector<uint8_t>>& levels)
        {
            const Layout layout(f, width, height, uint32_t(levels.size()));
            // The game's files end right after the last block any mip uses.
            uint32_t end = 0;
            for (uint32_t level = 0; level < levels.size(); ++level)
                for (uint32_t y = 0; y < Blocks(height >> level, f); ++y)
                    for (uint32_t x = 0; x < Blocks(width >> level, f); ++x)
                        end = std::max(end, layout.Address(level, x, y) + f.blockBytes);
            std::vector<uint8_t> out(end);
            for (uint32_t level = 0; level < levels.size(); ++level)
            {
                const uint32_t columns = Blocks(width >> level, f);
                for (uint32_t y = 0; y < Blocks(height >> level, f); ++y)
                    for (uint32_t x = 0; x < columns; ++x)
                    {
                        const uint8_t* src = levels[level].data() + (size_t(y) * columns + x) * f.blockBytes;
                        uint8_t* dst = out.data() + layout.Address(level, x, y);
                        // To the byte order the GPU un-swaps.
                        if (f.endian == 0)
                            memcpy(dst, src, f.blockBytes);
                        else
                        {
                            const uint32_t word = f.endian == 1 ? 2 : 4;
                            for (uint32_t i = 0; i < f.blockBytes; i += word)
                                for (uint32_t k = 0; k < word; ++k)
                                    dst[i + k] = src[i + word - 1 - k];
                        }
                    }
            }
            const uint32_t fetch[6] = {
                0x80000000u | (layout.PitchTexels() / 32) << 22 | 2,   // tiled, pitch, type 2 (texture)
                uint32_t(f.format) | f.endian << 6,
                (width - 1) | (height - 1) << 13,
                f.swizzle,
                uint32_t(levels.size() - 1) << 6,                     // max mip level
                1u << 9 | 1u << 11 | layout.MipAddress() / kPage << 12,   // 2D, packed mips, mip address
            };
            const uint32_t header[7] = { 3, 1, 0, 0, 0, 0xFFFF0000, 0xFFFF0000 };
            for (uint32_t v : header)
                for (int s = 24; s >= 0; s -= 8)
                    out.push_back(uint8_t(v >> s));
            for (uint32_t v : fetch)
                for (int s = 24; s >= 0; s -= 8)
                    out.push_back(uint8_t(v >> s));
            return out;
        }

        // Unpacks an uncompressed layout the 360 has no match for to A8R8G8B8 (PC byte
        // order); with `luminance`, the red mask is the grey of all three colours.
        std::vector<uint8_t> Expand(std::span<const uint8_t> data, uint32_t bytes, const std::array<uint32_t, 4>& masks,
            bool luminance)
        {
            const size_t count = data.size() / bytes;
            std::vector<uint8_t> out(count * 4);
            // B, G, R, A in memory.
            const int order[4] = { 2, 1, 0, 3 };
            for (size_t i = 0; i < count; ++i)
            {
                uint32_t v = 0;
                for (uint32_t k = 0; k < bytes; ++k)
                    v |= uint32_t(data[i * bytes + k]) << (8 * k);
                for (int c = 0; c < 4; ++c)
                {
                    const uint32_t mask = luminance && c < 3 ? masks[0] : masks[order[c]];
                    if (!mask)
                    {
                        out[i * 4 + c] = c == 3 ? 255 : 0;
                        continue;
                    }
                    const int shift = std::countr_zero(mask);
                    const uint32_t top = mask >> shift;
                    out[i * 4 + c] = uint8_t(std::nearbyint(double((v >> shift) & top) * 255 / top));
                }
            }
            return out;
        }

        // A BC4 block (as DXT5's alpha block) for 16 values: their range, each the
        // nearest of its 8 steps.
        void EncodeBc4(const uint8_t (&values)[16], uint8_t* out)
        {
            const auto [low, high] = std::minmax_element(std::begin(values), std::end(values));
            const int a0 = *high, a1 = *low;
            out[0] = uint8_t(a0);
            out[1] = uint8_t(a1);
            int steps[8] = { a0, a1 };
            for (int i = 2; i < 8; ++i)
                steps[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
            uint64_t indices = 0;
            for (int p = 0; p < 16; ++p)
            {
                int best = 0;
                for (int i = 1; i < 8 && a0 != a1; ++i)
                    if (std::abs(steps[i] - values[p]) < std::abs(steps[best] - values[p]))
                        best = i;
                indices |= uint64_t(best) << (3 * p);
            }
            for (int i = 0; i < 6; ++i)
                out[2 + i] = uint8_t(indices >> (8 * i));
        }

        // Whether DXT5 blocks hold a normal map the PC way: red the same throughout
        // (X is in alpha), alpha not.
        bool IsSwizzledNormalMap(std::span<const uint8_t> blocks)
        {
            int redLow = 31, redHigh = 0, alphaLow = 255, alphaHigh = 0;
            for (size_t at = 0; at + 16 <= blocks.size(); at += 16)
            {
                const uint8_t* block = blocks.data() + at;
                alphaLow = std::min({ alphaLow, int(block[0]), int(block[1]) });
                alphaHigh = std::max({ alphaHigh, int(block[0]), int(block[1]) });
                for (int c = 0; c < 2; ++c)
                {
                    const int red = block[9 + 2 * c] >> 3;
                    redLow = std::min(redLow, red);
                    redHigh = std::max(redHigh, red);
                }
            }
            constexpr int kRedSpread = 1, kAlphaSpread = 8;
            return redHigh - redLow <= kRedSpread && alphaHigh - alphaLow >= kAlphaSpread;
        }

        // DXT5 blocks with X in alpha and Y in green to DXN's (X's block, then Y's).
        std::vector<uint8_t> SwizzledNormalsToDxn(std::span<const uint8_t> blocks)
        {
            std::vector<uint8_t> out(blocks.size());
            for (size_t at = 0; at + 16 <= blocks.size(); at += 16)
            {
                const uint8_t* block = blocks.data() + at;
                memcpy(out.data() + at, block, 8);
                // Green from the colour block: its two 6-bit ends and the two steps between.
                const uint32_t c0 = block[8] | block[9] << 8, c1 = block[10] | block[11] << 8;
                auto green = [](uint32_t c) {
                    const int g = (c >> 5) & 63;
                    return g << 2 | g >> 4;
                };
                const int g0 = green(c0), g1 = green(c1);
                const int palette[4] = { g0, g1, (2 * g0 + g1) / 3, (g0 + 2 * g1) / 3 };
                const uint32_t indices = ui::ReadLE32(blocks, at + 12);
                uint8_t values[16];
                for (int p = 0; p < 16; ++p)
                    values[p] = uint8_t(palette[(indices >> (2 * p)) & 3]);
                EncodeBc4(values, out.data() + at + 8);
            }
            return out;
        }

        // DX10 header formats (DXGI_FORMAT values).
        enum Dxgi : uint32_t
        {
            kR8G8B8A8 = 28,
            kR8G8B8A8Srgb = 29,
            kR8 = 61,
            kA8 = 65,
            kBc1 = 71,
            kBc1Srgb = 72,
            kBc2 = 74,
            kBc2Srgb = 75,
            kBc3 = 77,
            kBc3Srgb = 78,
            kBc5 = 83,
            kB8G8R8A8 = 87,
            kB8G8R8X8 = 88,
            kB8G8R8A8Srgb = 91,
            kB8G8R8X8Srgb = 93,
            kB4G4R4A4 = 115,
        };

        // D3DFMT values some tools write as the FourCC.
        constexpr uint32_t kD3dArgb8 = 21, kD3dXrgb8 = 22, kD3dArgb4 = 26, kD3dL8 = 50;
    }

    std::variant<std::vector<uint8_t>, std::string> ConvertTexture(std::span<const uint8_t> dds,
        const TextureOptions& options)
    {
        constexpr uint32_t kFourcc = 0x4, kAlphaPixels = 0x1, kLuminance = 0x20000;
        constexpr uint32_t kCubemap = 0x200, kVolume = 0x200000;   // in caps2
        constexpr size_t kHeaderSize = 128, kDx10HeaderSize = 20;
        if (dds.size() < kHeaderSize || memcmp(dds.data(), "DDS ", 4) != 0)
            return std::string("it isn't a DDS file");
        // The header (DDS_HEADER, after the magic): height, width, mip count, then
        // the pixel format (flags, fourcc, bit count, red/green/blue/alpha masks)
        // and caps2.
        const uint32_t height = ui::ReadLE32(dds, 12), width = ui::ReadLE32(dds, 16);
        const uint32_t mipCount = std::max(ui::ReadLE32(dds, 28), 1u);
        const uint32_t pixelFlags = ui::ReadLE32(dds, 80);
        const uint32_t bits = ui::ReadLE32(dds, 88);
        std::array<uint32_t, 4> masks = { ui::ReadLE32(dds, 92), ui::ReadLE32(dds, 96), ui::ReadLE32(dds, 100),
            pixelFlags & kAlphaPixels ? ui::ReadLE32(dds, 104) : 0 };
        const uint32_t caps2 = ui::ReadLE32(dds, 112);
        if (caps2 & (kCubemap | kVolume))
            return std::string("cube and volume textures aren't handled");
        if (width == 0 || height == 0 || width > kMaxSize || height > kMaxSize)
            return fmt::format("its size ({}x{}) isn't one the Xbox 360 takes", width, height);
        const uint32_t levels = std::min(mipCount, uint32_t(std::bit_width(std::max(width, height))));

        const Format* format = nullptr;
        uint32_t expandBytes = 0;
        bool luminance = (pixelFlags & kLuminance) != 0;
        size_t offset = kHeaderSize;
        auto expand = [&](uint32_t bytes, std::array<uint32_t, 4> layout) {
            format = &kArgb8;
            expandBytes = bytes;
            masks = layout;
        };
        if (pixelFlags & kFourcc)
        {
            const std::string_view fourcc(reinterpret_cast<const char*>(dds.data() + 84), 4);
            const uint32_t code = ui::ReadLE32(dds, 84);
            if (fourcc == "DXT1")
                format = &kDxt1;
            else if (fourcc == "DXT2" || fourcc == "DXT3")
                format = &kDxt3;
            else if (fourcc == "DXT4" || fourcc == "DXT5")
                format = &kDxt5;
            else if (fourcc == "ATI2" || fourcc == "BC5U")
                format = &kDxn;
            else if (code == kD3dArgb8)
                format = &kArgb8;
            else if (code == kD3dXrgb8)
                format = &kXrgb8;
            else if (code == kD3dArgb4)
                format = &kArgb4;
            else if (code == kD3dL8)
                format = &kL8;
            else if (fourcc == "DX10")
            {
                if (dds.size() < kHeaderSize + kDx10HeaderSize)
                    return std::string("the file is cut short");
                offset += kDx10HeaderSize;
                constexpr uint32_t kTexture2D = 3, kCube = 0x4;
                const uint32_t dxgi = ui::ReadLE32(dds, 128), dimension = ui::ReadLE32(dds, 132);
                const uint32_t misc = ui::ReadLE32(dds, 136), arraySize = ui::ReadLE32(dds, 140);
                if (dimension != kTexture2D || (misc & kCube) || arraySize > 1)
                    return std::string("cube, volume and array textures aren't handled");
                switch (dxgi)
                {
                case kBc1:
                case kBc1Srgb:
                    format = &kDxt1;
                    break;
                case kBc2:
                case kBc2Srgb:
                    format = &kDxt3;
                    break;
                case kBc3:
                case kBc3Srgb:
                    format = &kDxt5;
                    break;
                case kBc5:
                    format = &kDxn;
                    break;
                case kB8G8R8A8:
                case kB8G8R8A8Srgb:
                    format = &kArgb8;
                    break;
                case kB8G8R8X8:
                case kB8G8R8X8Srgb:
                    format = &kXrgb8;
                    break;
                case kB4G4R4A4:
                    format = &kArgb4;
                    break;
                case kR8:
                    format = &kL8;
                    break;
                case kR8G8B8A8:
                case kR8G8B8A8Srgb:
                    expand(4, { 0xFF, 0xFF00, 0xFF0000, 0xFF000000 });
                    break;
                case kA8:
                    expand(1, { 0, 0, 0, 0xFF });
                    break;
                default:
                    return fmt::format("its format (DXGI {}) isn't handled", dxgi);
                }
            }
            else
                return fmt::format("its format ({}) isn't handled", fourcc);
        }
        else if (bits == 32 && masks == std::array<uint32_t, 4>{ 0xFF0000, 0xFF00, 0xFF, 0xFF000000 })
            format = &kArgb8;
        else if (bits == 32 && masks == std::array<uint32_t, 4>{ 0xFF0000, 0xFF00, 0xFF, 0 })
            format = &kXrgb8;
        else if (bits == 16 && masks == std::array<uint32_t, 4>{ 0xF00, 0xF0, 0xF, 0xF000 })
            format = &kArgb4;
        else if (bits == 8 && masks == std::array<uint32_t, 4>{ 0xFF, 0, 0, 0 })
            format = &kL8;
        else if (bits == 8 || bits == 16 || bits == 24 || bits == 32)
        {
            format = &kArgb8;
            expandBytes = bits / 8;
        }
        else
            return fmt::format("its {}-bit format isn't handled", bits);
        if (!expandBytes)
            luminance = false;

        std::vector<std::vector<uint8_t>> data;
        for (uint32_t level = 0; level < levels; ++level)
        {
            const uint32_t w = std::max(width >> level, 1u), h = std::max(height >> level, 1u);
            const size_t size = expandBytes ? size_t(w) * h * expandBytes
                                            : size_t(Blocks(w, *format)) * Blocks(h, *format) * format->blockBytes;
            if (size > dds.size() - offset)
                return std::string("the file is cut short");
            const auto levelData = dds.subspan(offset, size);
            data.push_back(expandBytes ? Expand(levelData, expandBytes, masks, luminance)
                                       : std::vector<uint8_t>(levelData.begin(), levelData.end()));
            offset += size;
        }
        if (options.normalMap && format == &kDxt5 && IsSwizzledNormalMap(data[0]))
        {
            format = &kDxn;
            for (auto& level : data)
                level = SwizzledNormalsToDxn(level);
        }
        const uint32_t drop = std::min(options.dropLevels, levels - 1);
        data.erase(data.begin(), data.begin() + drop);
        return Encode(*format, std::max(width >> drop, 1u), std::max(height >> drop, 1u), data);
    }
}
