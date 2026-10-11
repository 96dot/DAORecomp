#pragma once
// PC meshes (.msh) to the Xbox 360's vertex formats.
//
// The 360 version's vertex shaders expect normals, tangents and bone weights as
// UB4N bytes (normals and tangents remapped from [-1, 1], the tangent's w the
// bitangent's handedness, no bitangent), bone indices as UB4, and the PC's
// positions and texture coordinates as they are. A UB4N/UB4 element is a 32-bit
// word with x in its low byte, so in the big-endian files its bytes are w, z, y, x.
// Halfs and floats are big-endian; the index buffer is big-endian u16s.

#include "mods/convert/gff.h"

namespace dao::mods::convert
{
    // Re-encodes a PC mesh's vertex and index buffers in place (the GFF tree, to
    // be written with the 360's templates). An error message if it can't.
    std::optional<std::string> ConvertMesh(gff::File& mesh);
}
