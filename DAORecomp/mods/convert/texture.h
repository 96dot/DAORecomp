#pragma once
// PC textures (.dds) to the Xbox 360's (.xds): the texels tiled for the 360's GPU
// with its packed mip tail (laid out by Xenia's texture_util), in the byte order
// its fetch constant un-swaps, then the 52-byte D3D texture header the game reads:
// Common 3, RefCount 1, Fence/ReadFence/Identifier 0, BaseFlush and MipFlush
// 0xFFFF0000, and the six-dword fetch constant (addresses as page offsets in the file).
//
// DXT1/3/5 and BC5 (DXN) keep their blocks, A8R8G8B8, X8R8G8B8, A4R4G4B4 and L8
// keep their texels, and other uncompressed layouts are expanded to A8R8G8B8. Both
// the old header and the DX10 one (DXGI formats) are read.

#include <variant>

namespace dao::mods::convert
{
    struct TextureOptions
    {
        // A normal map: one stored the PC way in DXT5 (X in alpha, Y in green, red
        // and blue unused) becomes DXN, which the 360's shaders read X and Y from.
        bool normalMap = false;
        // Leaves out this many of the largest mips, as far as the file has smaller
        // ones (the 360's smaller copies for distant models are half and quarter size).
        uint32_t dropLevels = 0;
    };

    // The .xds file, or an error message.
    std::variant<std::vector<uint8_t>, std::string> ConvertTexture(std::span<const uint8_t> dds,
        const TextureOptions& options = {});
}
