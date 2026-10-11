#include "stdafx.h"
#include "mods/convert/mesh.h"

#include <cmath>

namespace dao::mods::convert
{
    namespace
    {
        // Mesh fields.
        enum : uint32_t
        {
            kChunks = 8021,
            kVertexData = 8022,
            kIndexData = 8023,
            kVertexSize = 8000,
            kVertexCount = 8001,
            kIndexCount = 8002,
            kVertexOffset = 8006,
            kIndexStart = 8009,
            kDeclaration = 8025,
            kX360Only = 8035,
            kDeclStream = 8026,
            kDeclOffset = 8027,
            kDeclType = 8028,
            kDeclUsage = 8029,
            kDeclMethod = 8030,
            kDeclUsageIndex = 8031,
        };

        // A declaration's stream and type in its end row (D3DDECL_END).
        constexpr uint32_t kDeclEnd = 0xFFFFFFFF;

        // D3DDECLTYPE
        enum DeclType : uint32_t
        {
            kF1, kF2, kF3, kF4, kColor, kUB4, kS2, kS4, kUB4N, kS2N, kS4N, kUS2N, kUS4N, kUDec3, kDec3N, kH2, kH4,
        };
        // D3DDECLUSAGE
        enum Usage : uint32_t
        {
            kPosition, kBlendWeight, kBlendIndices, kNormal, kPointSize, kTexCoord, kTangent, kBinormal, kTessFactor,
            kPositionT, kVertexColor,
        };

        uint32_t TypeSize(uint32_t type)
        {
            static constexpr uint32_t sizes[] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 0, 0, 4, 8 };
            return type < std::size(sizes) ? sizes[type] : 0;
        }

        double HalfToDouble(uint16_t h)
        {
            const int exponent = (h >> 10) & 0x1F;
            const int mantissa = h & 0x3FF;
            double v;
            if (exponent == 0)
                v = std::ldexp(mantissa, -24);
            else if (exponent == 31)
                v = mantissa ? NAN : INFINITY;
            else
                v = std::ldexp(mantissa | 0x400, exponent - 25);
            return (h & 0x8000) ? -v : v;
        }

        // Round to nearest even, like numpy's float64 to float16.
        uint16_t DoubleToHalf(double v)
        {
            const uint16_t sign = std::signbit(v) ? 0x8000 : 0;
            v = std::fabs(v);
            if (std::isnan(v))
                return sign | 0x7E00;
            if (v >= 65520.0)
                return sign | 0x7C00;
            int exponent;
            std::frexp(v, &exponent);   // v = m * 2^exponent, m in [0.5, 1)
            if (v < std::ldexp(1.0, -14))
            {
                // Subnormal: a multiple of 2^-24.
                const double units = std::nearbyint(std::ldexp(v, 24));
                return sign | uint16_t(units);   // 1024 units rolls into the smallest normal
            }
            // 11 significant bits.
            const double scaled = std::nearbyint(std::ldexp(v, 11 - exponent));
            int e = exponent - 1 + 15;
            uint32_t m = uint32_t(scaled);
            if (m == 2048)
            {
                m = 1024;
                ++e;
            }
            if (e >= 31)
                return sign | 0x7C00;
            return sign | uint16_t(e << 10) | uint16_t(m & 0x3FF);
        }

        using Vec4 = std::array<double, 4>;

        // One element of every vertex, as the GPU reads it (before any shader remapping).
        std::vector<Vec4> Decode(std::span<const uint8_t> vb, size_t base, uint32_t stride, uint32_t count,
            uint32_t type, uint32_t offset)
        {
            std::vector<Vec4> out(count, Vec4{ 0, 0, 0, 1 });
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t* p = vb.data() + base + size_t(i) * stride + offset;
                Vec4& v = out[i];
                auto i16 = [&](int k) { int16_t x; memcpy(&x, p + 2 * k, 2); return double(x); };
                auto u16 = [&](int k) { uint16_t x; memcpy(&x, p + 2 * k, 2); return double(x); };
                switch (type)
                {
                case kF1: case kF2: case kF3: case kF4:
                    for (uint32_t k = 0; k <= type; ++k)
                    {
                        float f;
                        memcpy(&f, p + 4 * k, 4);
                        v[k] = f;
                    }
                    break;
                case kH2: case kH4:
                    for (int k = 0; k < (type == kH2 ? 2 : 4); ++k)
                    {
                        uint16_t h;
                        memcpy(&h, p + 2 * k, 2);
                        v[k] = HalfToDouble(h);
                    }
                    break;
                case kS2: case kS4:
                    for (int k = 0; k < (type == kS2 ? 2 : 4); ++k)
                        v[k] = i16(k);
                    break;
                case kS2N: case kS4N:
                    for (int k = 0; k < (type == kS2N ? 2 : 4); ++k)
                        v[k] = std::max(i16(k) / 32767, -1.0);
                    break;
                case kUS2N: case kUS4N:
                    for (int k = 0; k < (type == kUS2N ? 2 : 4); ++k)
                        v[k] = u16(k) / 65535;
                    break;
                case kUB4: case kUB4N:
                    for (int k = 0; k < 4; ++k)
                        v[k] = type == kUB4N ? p[k] / 255.0 : p[k];
                    break;
                case kColor:
                    // D3DCOLOR: an ARGB word, read as (r, g, b, a).
                    v = { p[2] / 255.0, p[1] / 255.0, p[0] / 255.0, p[3] / 255.0 };
                    break;
                }
            }
            return out;
        }

        uint8_t Byte(double v)
        {
            return uint8_t(std::clamp(std::nearbyint(v), 0.0, 255.0));
        }

        struct Column
        {
            uint32_t usage, index, type;
            std::vector<uint8_t> data;   // per vertex, TypeSize(type) bytes
        };

        // A 32-bit word with x in its low byte, big-endian: w, z, y, x.
        void PutWord(std::vector<uint8_t>& out, double x, double y, double z, double w)
        {
            out.insert(out.end(), { Byte(w), Byte(z), Byte(y), Byte(x) });
        }

        void PutFloat(std::vector<uint8_t>& out, double v)
        {
            const uint32_t bits = ByteSwap(std::bit_cast<uint32_t>(float(v)));
            const auto* b = reinterpret_cast<const uint8_t*>(&bits);
            out.insert(out.end(), b, b + 4);
        }

        void PutHalf(std::vector<uint8_t>& out, double v)
        {
            const uint16_t h = DoubleToHalf(v);
            out.insert(out.end(), { uint8_t(h >> 8), uint8_t(h) });
        }

        std::array<double, 3> Unit(const Vec4& v)
        {
            const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-12;
            return { v[0] / length, v[1] / length, v[2] / length };
        }

        gff::Value Prim(uint16_t type, uint32_t value)
        {
            gff::Value v;
            v.type = type;
            v.SetU32(value);
            return v;
        }

        struct Element
        {
            uint32_t type, offset;
        };

        // A chunk's vertices in the 360's formats: the bytes, the vertex size and the declaration rows.
        std::optional<std::string> ConvertVertices(std::span<const uint8_t> vb, gff::Value& chunk,
            std::vector<uint8_t>& out, uint32_t& stride, std::vector<std::array<uint32_t, 6>>& decl)
        {
            const uint32_t inStride = chunk.At(kVertexSize).U32();
            const uint32_t count = chunk.At(kVertexCount).U32();
            const size_t base = chunk.At(kVertexOffset).U32();
            if (inStride == 0 || base > vb.size() || uint64_t(inStride) * count > vb.size() - base)
                return "its vertices are outside the vertex data";

            std::map<std::pair<uint32_t, uint32_t>, Element> elements;   // (usage, index)
            for (const gff::Value& row : chunk.At(kDeclaration).items)
            {
                const uint32_t stream = row.Find(kDeclStream) ? row.Find(kDeclStream)->U32() : 0;
                if (stream == kDeclEnd)
                    continue;
                if (stream != 0)
                    return "it has more than one vertex stream";
                const gff::Value* type = row.Find(kDeclType);
                const gff::Value* offset = row.Find(kDeclOffset);
                const gff::Value* usage = row.Find(kDeclUsage);
                const gff::Value* index = row.Find(kDeclUsageIndex);
                if (!type || !offset || !usage || !index)
                    return "its vertex declaration is incomplete";
                const uint32_t size = TypeSize(type->U32());
                if (size == 0)
                    return fmt::format("vertex element type {} isn't handled", type->U32());
                if (offset->U32() > inStride || size > inStride - offset->U32())
                    return "a vertex element is outside the vertex";
                elements[{ usage->U32(), index->U32() }] = { type->U32(), offset->U32() };
            }
            for (const auto& [key, element] : elements)
            {
                const uint32_t usage = key.first;
                if (usage != kPosition && usage != kBlendWeight && usage != kBlendIndices && usage != kNormal &&
                    usage != kTangent && usage != kBinormal && usage != kTexCoord && usage != kVertexColor)
                    return fmt::format("vertex usage {} isn't handled", usage);
            }
            auto decode = [&](uint32_t usage, uint32_t index = 0) {
                const Element& e = elements.at({ usage, index });
                return Decode(vb, base, inStride, count, e.type, e.offset);
            };
            auto has = [&](uint32_t usage, uint32_t index = 0) { return elements.contains({ usage, index }); };
            if (!has(kPosition))
                return "it has no positions";

            std::vector<Column> columns;
            {
                const uint32_t type = elements.at({ kPosition, 0 }).type;
                const auto pos = decode(kPosition);
                Column column{ kPosition, 0, type == kF3 || type == kF4 ? type : uint32_t(kH4) };
                for (const Vec4& v : pos)
                    for (uint32_t k = 0; k < (column.type == kF3 ? 3u : 4u); ++k)
                        column.type == kH4 ? PutHalf(column.data, v[k]) : PutFloat(column.data, v[k]);
                columns.push_back(std::move(column));
            }
            if (has(kBlendWeight))
            {
                if (!has(kBlendIndices))
                    return "it has bone weights without bone indices";
                Column weights{ kBlendWeight, 0, kUB4N };
                for (const Vec4& w : decode(kBlendWeight))
                {
                    // Bytes that still sum to 255.
                    std::array<int, 4> b;
                    for (int k = 0; k < 4; ++k)
                        b[k] = int(std::clamp(std::nearbyint(w[k] * 255), 0.0, 255.0));
                    const int top = int(std::ranges::max_element(b) - b.begin());
                    b[top] = std::clamp(b[top] + 255 - (b[0] + b[1] + b[2] + b[3]), 0, 255);
                    PutWord(weights.data, b[0], b[1], b[2], b[3]);
                }
                columns.push_back(std::move(weights));
                Column indices{ kBlendIndices, 0, kUB4 };
                for (const Vec4& i : decode(kBlendIndices))
                    PutWord(indices.data, i[0], i[1], i[2], i[3]);
                columns.push_back(std::move(indices));
            }
            if (has(kNormal))
            {
                const auto normals = decode(kNormal);
                Column column{ kNormal, 0, kUB4N };
                std::vector<std::array<double, 3>> units(count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    units[i] = Unit(normals[i]);
                    const auto& n = units[i];
                    PutWord(column.data, n[0] * 127.5 + 127.5, n[1] * 127.5 + 127.5, n[2] * 127.5 + 127.5, 255);
                }
                columns.push_back(std::move(column));
                if (has(kTangent))
                {
                    const auto tangents = decode(kTangent);
                    const auto binormals = has(kBinormal) ? decode(kBinormal) : std::vector<Vec4>();
                    Column tangent{ kTangent, 0, kUB4N };
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        const auto t = Unit(tangents[i]);
                        const auto& n = units[i];
                        double hand;
                        if (!binormals.empty())
                        {
                            const auto& b = binormals[i];
                            const double dot = (n[1] * t[2] - n[2] * t[1]) * b[0] + (n[2] * t[0] - n[0] * t[2]) * b[1] +
                                (n[0] * t[1] - n[1] * t[0]) * b[2];
                            hand = dot < 0 ? 0 : 255;
                        }
                        else
                            hand = tangents[i][3] < 0 ? 0 : 255;
                        PutWord(tangent.data, t[0] * 127.5 + 127.5, t[1] * 127.5 + 127.5, t[2] * 127.5 + 127.5, hand);
                    }
                    columns.push_back(std::move(tangent));
                }
            }
            for (const auto& [key, element] : elements)
            {
                if (key.first != kTexCoord)
                    continue;
                const bool isFloat = element.type == kF2 || element.type == kF3 || element.type == kF4;
                Column column{ kTexCoord, key.second, isFloat ? element.type : uint32_t(kH2) };
                const uint32_t n = isFloat ? element.type + 1 : 2;
                for (const Vec4& uv : decode(kTexCoord, key.second))
                    for (uint32_t k = 0; k < n; ++k)
                        isFloat ? PutFloat(column.data, uv[k]) : PutHalf(column.data, uv[k]);
                columns.push_back(std::move(column));
            }
            for (const auto& [key, element] : elements)
            {
                if (key.first != kVertexColor)
                    continue;
                // An ARGB word, big-endian: a, r, g, b.
                Column column{ kVertexColor, key.second, kColor };
                for (const Vec4& c : decode(kVertexColor, key.second))
                    for (int k : { 3, 0, 1, 2 })
                        column.data.push_back(Byte(std::clamp(c[k], 0.0, 1.0) * 255));
                columns.push_back(std::move(column));
            }

            stride = 0;
            for (const Column& column : columns)
            {
                decl.push_back({ 0, stride, column.type, column.usage, 0, column.index });
                stride += TypeSize(column.type);
            }
            out.reserve(out.size() + size_t(stride) * count);
            for (uint32_t i = 0; i < count; ++i)
                for (const Column& column : columns)
                {
                    const uint32_t size = TypeSize(column.type);
                    const auto from = column.data.begin() + size_t(i) * size;
                    out.insert(out.end(), from, from + size);
                }
            return std::nullopt;
        }
    }

    std::optional<std::string> ConvertMesh(gff::File& mesh)
    {
        gff::Value& root = mesh.root;
        // (Found, not At: adding a field would move the others.)
        gff::Value* chunks = root.Find(kChunks);
        if (!chunks || chunks->items.empty())
            return std::nullopt;
        static const std::vector<uint8_t> kNone;
        const gff::Value* vertexBytes = root.Find(kVertexData);
        const gff::Value* indexBytes = root.Find(kIndexData);
        const std::vector<uint8_t>& vb = vertexBytes ? vertexBytes->bytes : kNone;
        const std::vector<uint8_t>& ib = indexBytes ? indexBytes->bytes : kNone;

        // Laid out as the game's own meshes are: each chunk's vertices from a multiple
        // of 16 bytes; after the last, padding to the next multiple of 16 (a whole 16
        // when it ends on one), and after the indices to the next multiple of 32.
        std::vector<uint8_t> vertices;
        uint64_t indexEnd = 0;
        for (size_t i = 0; i < chunks->items.size(); ++i)
        {
            gff::Value& chunk = chunks->items[i];
            const gff::Value& declaration = chunk.At(kDeclaration);
            if (chunk.kind != gff::Value::Kind::Struct || declaration.items.empty())
                return fmt::format("chunk {} has no vertex declaration", i);
            // The rows' struct, for the new rows.
            const uint32_t rowSid = declaration.items[0].sid;
            const gff::Fourcc rowFourcc = declaration.items[0].fourcc;
            uint32_t stride = 0;
            std::vector<std::array<uint32_t, 6>> rows;
            while (vertices.size() % 16)
                vertices.push_back(0);
            const size_t offset = vertices.size();
            if (auto error = ConvertVertices(vb, chunk, vertices, stride, rows))
                return fmt::format("chunk {}: {}", i, *error);
            chunk.At(kVertexOffset).SetU32(uint32_t(offset));
            chunk.At(kVertexSize).SetU32(stride);

            rows.push_back({ kDeclEnd, 0, kDeclEnd, kDeclEnd, 0, 0 });
            gff::Value list;
            list.kind = gff::Value::Kind::List;
            for (const auto& row : rows)
            {
                gff::Value element;
                element.kind = gff::Value::Kind::Struct;
                element.sid = rowSid;
                element.fourcc = rowFourcc;
                // Stream and offset are i32s, the rest u32s.
                const uint32_t labels[] = { kDeclStream, kDeclOffset, kDeclType, kDeclUsage, kDeclMethod,
                    kDeclUsageIndex };
                for (int k = 0; k < 6; ++k)
                    element.fields.push_back({ labels[k], Prim(k < 2 ? gff::kI32 : gff::kU32, row[k]) });
                list.items.push_back(std::move(element));
            }
            chunk.At(kDeclaration) = std::move(list);
            chunk.At(kX360Only) = gff::Value();
            indexEnd = std::max(indexEnd, (uint64_t(chunk.At(kIndexStart).U32()) + chunk.At(kIndexCount).U32()) * 2);
        }
        vertices.resize(vertices.size() + 16 - vertices.size() % 16);
        if (indexEnd > ib.size())
            return "its indices are outside the index data";
        std::vector<uint8_t> indices(ib.begin(), ib.begin() + size_t(indexEnd));
        for (size_t i = 0; i + 1 < indices.size(); i += 2)
            std::swap(indices[i], indices[i + 1]);
        indices.resize(indices.size() + 32 - indices.size() % 32);

        gff::Value& vertexData = root.At(kVertexData);
        vertexData.kind = gff::Value::Kind::Bytes;
        vertexData.bytes = std::move(vertices);
        gff::Value& indexData = root.At(kIndexData);
        indexData.kind = gff::Value::Kind::Bytes;
        indexData.bytes = std::move(indices);
        return std::nullopt;
    }
}
