#pragma once
// GFF V4.0, the format of the game's models, meshes, tables, talk tables and most
// other data: a header, struct templates (fourcc, size, fields), then the data.
// The PC version's files are little-endian and the Xbox 360 version's big-endian,
// with struct fields at different offsets, so converting one means reading the
// tree and writing it again.
//
// Header: "GFF V4.0", platform ("PC  " or "X360"), file type, type version,
// struct count, data offset. Then 16-byte structs (fourcc, field count, field
// table offset, size) and 12-byte fields (label, type | flags << 16, offset).
// Strings are a length and UTF-16 code units; references and lists are offsets
// from the data offset (lists start with a count); -1 is null.

namespace dao::mods::gff
{
    enum Flags : uint16_t
    {
        kList = 0x8000,
        kStruct = 0x4000,
        kRef = 0x2000,
    };

    enum Types : uint16_t
    {
        kU8 = 0,
        kF32 = 8,
        kVec4 = 12,
        kQuat = 13,
        kColor = 15,
        kMatrix = 16,
        kString = 14,
        kTlk = 17,
        kGeneric = 0xFFFF,
    };

    using Fourcc = std::array<char, 4>;

    struct FieldDef
    {
        uint32_t label;
        uint16_t type;   // a struct id when flags has kStruct
        uint16_t flags;
        uint32_t offset;
    };

    struct Template
    {
        Fourcc fourcc;
        uint32_t size;
        std::vector<FieldDef> fields;   // sorted by label

        const FieldDef* Find(uint32_t label) const;
    };

    struct Field;

    struct Value
    {
        enum class Kind : uint8_t
        {
            Null,
            Prim,      // bytes: the value, little-endian components
            String,    // text
            Tlk,       // tlkId; text if hasText, else tlkRaw
            Bytes,     // bytes: a u8 list
            List,      // items
            Struct,    // sid, fourcc, fields
            Generic,   // type and flags; items[0] if not null
        };

        Kind kind = Kind::Null;
        uint16_t type = 0;
        uint16_t flags = 0;
        std::vector<uint8_t> bytes;
        std::u16string text;
        uint32_t tlkId = 0;
        uint32_t tlkRaw = 0;
        bool hasText = false;
        std::vector<Value> items;
        uint32_t sid = 0;
        Fourcc fourcc{};
        std::vector<Field> fields;

        // A struct's field, or null.
        Value* Find(uint32_t label);
        const Value* Find(uint32_t label) const;
        // A struct field's value (added as null if missing).
        Value& At(uint32_t label);

        // A u32/i32 primitive (0 for anything else).
        uint32_t U32() const;
        void SetU32(uint32_t value);
    };

    struct Field
    {
        uint32_t label;
        Value value;
    };

    struct File
    {
        Fourcc platform{};
        Fourcc type{};
        Fourcc version{};
        std::vector<Template> templates;
        Value root;

        bool IsX360() const { return platform == Fourcc{ 'X', '3', '6', '0' }; }
    };

    // The struct templates alone (for building the 360 layouts from the game's files).
    std::optional<std::vector<Template>> ReadTemplates(std::span<const uint8_t> data);

    // Nothing if it isn't a well-formed GFF V4.0 file.
    std::optional<File> Read(std::span<const uint8_t> data);

    // `file` in the 360's byte order with `templates` (template i is struct id i of
    // the file's own; the first is the root's).
    std::vector<uint8_t> WriteX360(const File& file, const std::vector<Template>& templates);

    // Bytes a value of (type, flags) takes in its slot.
    uint32_t SlotSize(uint16_t type, uint16_t flags, const std::vector<Template>& templates);
    // A struct's alignment: 16 if it holds 16-byte values (directly or in inline structs), else 4.
    uint32_t StructAlign(const Template& tpl, const std::vector<Template>& templates, int depth = 0);
    // A primitive's size, or 0 if `type` isn't one.
    uint32_t PrimSize(uint16_t type);
}
