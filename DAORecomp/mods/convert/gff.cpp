#include "stdafx.h"
#include "mods/convert/gff.h"

#include <stdexcept>

namespace dao::mods::gff
{
    namespace
    {
        constexpr uint32_t kNull = 0xFFFFFFFF;
        constexpr size_t kHeaderSize = 28;
        constexpr size_t kStructEntrySize = 16;
        constexpr size_t kFieldEntrySize = 12;
        // Mod files are untrusted: references can point anywhere, including back up the tree.
        constexpr int kMaxDepth = 64;
        // The memory the tree read from a file may take (values cost their size,
        // strings and byte lists their bytes): as many values as the file has bytes,
        // so references sharing data can't blow it up, and at most this.
        constexpr size_t kMaxTreeBytes = size_t(1) << 30;

        struct Malformed : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        // Component size of a primitive (its byte order is swapped per component).
        uint32_t ComponentSize(uint16_t type)
        {
            switch (type)
            {
            case kU8: case kI8: return 1;
            case kU16: case kI16: return 2;
            case kU64: case kI64: case kF64: return 8;
            default: return 4;
            }
        }

        void Swap(uint8_t* data, uint32_t size, uint32_t component)
        {
            for (uint32_t i = 0; i + component <= size; i += component)
                std::reverse(data + i, data + i + component);
        }

        // The flags a value of (type, flags) is read and written with: a generic field
        // is always an inline (typeflags, ref) pair.
        uint16_t SlotFlags(uint16_t type, uint16_t flags)
        {
            return type == kGeneric && !(flags & kList) ? 0 : flags;
        }

        // A list's items' flags: generic list elements are (typeflags, ref) pairs
        // whatever the field's other flags say.
        uint16_t ItemFlags(uint16_t type, uint16_t flags)
        {
            return type == kGeneric ? 0 : uint16_t(flags & ~kList);
        }

        // A list of u8s is packed: its count, then its bytes (Value::Kind::Bytes).
        bool IsByteList(uint16_t type, uint16_t itemFlags)
        {
            return type == kU8 && !itemFlags;
        }

        class Reader
        {
        public:
            Reader(std::span<const uint8_t> data, bool bigEndian, uint32_t dataOffset,
                const std::vector<Template>& templates)
                : data_(data), bigEndian_(bigEndian), dataOffset_(dataOffset), templates_(templates),
                  budget_(std::min(data.size(), kMaxTreeBytes / sizeof(Value)) * sizeof(Value))
            {
            }

            Value Read(uint16_t type, uint16_t flags, uint64_t at, int depth)
            {
                if (depth > kMaxDepth)
                    throw Malformed("nested too deeply");
                Spend(sizeof(Value));
                flags = SlotFlags(type, flags);

                Value value;
                if (flags & kList)
                {
                    const uint32_t ref = U32(at);
                    if (ref == kNull)
                        return value;
                    const uint64_t base = uint64_t(dataOffset_) + ref;
                    const uint32_t count = U32(base);
                    const uint16_t sub = ItemFlags(type, flags);
                    if (IsByteList(type, sub))
                    {
                        Need(base + 4, count);
                        Spend(count);
                        value.kind = Value::Kind::Bytes;
                        value.bytes.assign(data_.begin() + size_t(base + 4), data_.begin() + size_t(base + 4 + count));
                        return value;
                    }
                    const uint32_t size = SlotSize(type, sub, templates_);
                    if (size == 0)
                        throw Malformed("empty list element");
                    Need(base + 4, uint64_t(count) * size);
                    value.kind = Value::Kind::List;
                    value.type = type;
                    value.flags = flags;
                    // Each item charges the budget as it's read, so nothing is reserved ahead: nested
                    // lists would each reserve against the same budget (gigabytes from a megabyte).
                    if (budget_ / sizeof(Value) < count)
                        throw Malformed("too big");
                    for (uint32_t i = 0; i < count; ++i)
                        value.items.push_back(Read(type, sub, base + 4 + uint64_t(i) * size, depth + 1));
                    return value;
                }
                if (flags & kRef)
                {
                    const uint32_t ref = U32(at);
                    if (ref == kNull)
                        return value;
                    return Read(type, uint16_t(flags & ~kRef), uint64_t(dataOffset_) + ref, depth + 1);
                }
                if (flags & kStruct)
                {
                    const Template& tpl = TemplateAt(type);
                    Need(at, tpl.size);
                    value.kind = Value::Kind::Struct;
                    value.sid = type;
                    value.fourcc = tpl.fourcc;
                    for (const FieldDef& field : tpl.fields)
                        value.fields.push_back(
                            { field.label, Read(field.type, field.flags, at + field.offset, depth + 1) });
                    return value;
                }
                if (type == kString)
                {
                    const uint32_t ref = U32(at);
                    if (ref != kNull)
                    {
                        value.kind = Value::Kind::String;
                        value.text = String(ref);
                    }
                    return value;
                }
                if (type == kTlk)
                {
                    // (talk table id, string ref); a ref of 0 or -1 means no inline string.
                    value.kind = Value::Kind::Tlk;
                    value.tlkId = U32(at);
                    const uint32_t ref = U32(at + 4);
                    value.hasText = ref != 0 && ref != kNull;
                    if (value.hasText)
                        value.text = String(ref);
                    else
                        value.tlkRaw = ref;
                    return value;
                }
                if (type == kGeneric)
                {
                    const uint32_t typeFlags = U32(at);
                    const uint32_t ref = U32(at + 4);
                    value.kind = Value::Kind::Generic;
                    value.type = uint16_t(typeFlags);
                    value.flags = uint16_t(typeFlags >> 16);
                    if (ref != kNull)
                        value.items.push_back(Read(value.type, value.flags, uint64_t(dataOffset_) + ref, depth + 1));
                    return value;
                }
                const uint32_t size = PrimSize(type);
                if (size == 0)
                    throw Malformed(fmt::format("unknown field type {}", type));
                Need(at, size);
                value.kind = Value::Kind::Prim;
                value.type = type;
                value.bytes.assign(data_.begin() + size_t(at), data_.begin() + size_t(at + size));
                if (bigEndian_)
                    Swap(value.bytes.data(), size, ComponentSize(type));
                return value;
            }

        private:
            // Charges the budget (see kMaxTreeBytes).
            void Spend(size_t bytes)
            {
                if (budget_ < bytes)
                    throw Malformed("too big");
                budget_ -= bytes;
            }

            void Need(uint64_t at, uint64_t size) const
            {
                if (at > data_.size() || size > data_.size() - at)
                    throw Malformed("reference out of the file");
            }

            uint32_t U32(uint64_t at) const
            {
                Need(at, 4);
                uint32_t v;
                memcpy(&v, data_.data() + at, 4);
                return bigEndian_ ? ByteSwap(v) : v;
            }

            const Template& TemplateAt(uint16_t id) const
            {
                if (id >= templates_.size())
                    throw Malformed("unknown struct");
                return templates_[id];
            }

            std::u16string String(uint32_t ref)
            {
                const uint64_t at = uint64_t(dataOffset_) + ref;
                const uint32_t count = U32(at);
                Need(at + 4, uint64_t(count) * 2);
                Spend(size_t(count) * 2);
                // Code unit by code unit: some of the game's strings aren't valid UTF-16, and they must survive.
                std::u16string text(count, u'\0');
                memcpy(text.data(), data_.data() + at + 4, size_t(count) * 2);
                if (bigEndian_)
                    for (char16_t& c : text)
                        c = ByteSwap(c);
                return text;
            }

            std::span<const uint8_t> data_;
            bool bigEndian_;
            uint32_t dataOffset_;
            const std::vector<Template>& templates_;
            size_t budget_;
        };

        class Writer
        {
        public:
            explicit Writer(const std::vector<Template>& templates) : templates_(templates) {}

            std::vector<uint8_t> out;

            size_t Alloc(size_t size, size_t align = 4)
            {
                while (out.size() % align)
                    out.push_back(0);
                const size_t at = out.size();
                out.resize(at + size);
                return at;
            }

            void Put32(size_t at, uint32_t v)
            {
                Check(at, 4);
                v = ByteSwap(v);
                memcpy(out.data() + at, &v, 4);
            }

            void Fill(uint16_t type, uint16_t flags, const Value& v, size_t at, int depth)
            {
                if (depth > kMaxDepth)
                    throw Malformed("nested too deeply");
                flags = SlotFlags(type, flags);
                if (flags & kList)
                {
                    if (v.kind == Value::Kind::Null)
                        return Put32(at, kNull);
                    const uint16_t sub = ItemFlags(type, flags);
                    if (IsByteList(type, sub))
                    {
                        const std::vector<uint8_t> empty;
                        const std::vector<uint8_t>& bytes = v.kind == Value::Kind::Bytes ? v.bytes : empty;
                        const size_t base = Alloc(4 + bytes.size());
                        Put32(base, uint32_t(bytes.size()));
                        std::ranges::copy(bytes, out.begin() + base + 4);
                        return Put32(at, uint32_t(base));
                    }
                    const uint32_t size = SlotSize(type, sub, templates_);
                    // Inline structs in a list start after its 4-byte count, so align the count's end.
                    const size_t align = BlockAlign(type, sub);
                    Alloc(0);
                    const size_t pad = (align - (out.size() + 4) % align) % align;
                    const size_t base = Alloc(pad + 4 + size_t(size) * v.items.size()) + pad;
                    Put32(base, uint32_t(v.items.size()));
                    Put32(at, uint32_t(base));
                    for (size_t i = 0; i < v.items.size(); ++i)
                        Fill(type, sub, v.items[i], base + 4 + i * size, depth + 1);
                    return;
                }
                if (flags & kRef)
                {
                    if (v.kind == Value::Kind::Null)
                        return Put32(at, kNull);
                    const uint16_t sub = uint16_t(flags & ~kRef);
                    const size_t base = Alloc(SlotSize(type, sub, templates_), BlockAlign(type, sub));
                    Put32(at, uint32_t(base));
                    return Fill(type, sub, v, base, depth + 1);
                }
                if (flags & kStruct)
                {
                    if (type >= templates_.size())
                        throw Malformed("unknown struct");
                    static const Value null;
                    for (const FieldDef& field : templates_[type].fields)
                    {
                        const Value* fieldValue = v.Find(field.label);
                        Fill(field.type, field.flags, fieldValue ? *fieldValue : null, at + field.offset, depth + 1);
                    }
                    return;
                }
                if (type == kString)
                    return WriteString(v.kind == Value::Kind::String ? &v.text : nullptr, at);
                if (type == kTlk)
                {
                    if (v.kind != Value::Kind::Tlk)
                    {
                        Put32(at, kNull);
                        return Put32(at + 4, kNull);
                    }
                    Put32(at, v.tlkId);
                    if (v.hasText)
                        return WriteString(&v.text, at + 4);
                    return Put32(at + 4, v.tlkRaw);
                }
                if (type == kGeneric)
                {
                    if (v.kind != Value::Kind::Generic)
                    {
                        Put32(at, 0);
                        return Put32(at + 4, kNull);
                    }
                    Put32(at, v.type | uint32_t(v.flags) << 16);
                    if (v.items.empty())
                        return Put32(at + 4, kNull);
                    const size_t base = Alloc(SlotSize(v.type, v.flags, templates_), BlockAlign(v.type, v.flags));
                    Put32(at + 4, uint32_t(base));
                    return Fill(v.type, v.flags, v.items[0], base, depth + 1);
                }
                const uint32_t size = PrimSize(type);
                if (size == 0)
                    throw Malformed(fmt::format("unknown field type {}", type));
                Check(at, size);
                if (v.kind != Value::Kind::Prim)
                    return;   // zero
                if (v.bytes.size() != size)
                    throw Malformed("primitive of the wrong size");
                std::ranges::copy(v.bytes, out.begin() + at);
                Swap(out.data() + at, size, ComponentSize(type));
            }

        private:
            void Check(size_t at, size_t size) const
            {
                if (at > out.size() || size > out.size() - at)
                    throw Malformed("field outside its struct");
            }

            size_t BlockAlign(uint16_t type, uint16_t flags) const
            {
                if ((flags & kStruct) && !(flags & (kList | kRef)) && type < templates_.size())
                    return StructAlign(templates_[type], templates_);
                return 4;
            }

            void WriteString(const std::u16string* text, size_t at)
            {
                if (!text)
                    return Put32(at, kNull);
                const size_t base = Alloc(4 + text->size() * 2);
                Put32(base, uint32_t(text->size()));
                for (size_t i = 0; i < text->size(); ++i)
                {
                    const char16_t c = (*text)[i];
                    out[base + 4 + i * 2] = uint8_t(c >> 8);
                    out[base + 5 + i * 2] = uint8_t(c);
                }
                Put32(at, uint32_t(base));
            }

            const std::vector<Template>& templates_;
        };

        uint32_t Get32(std::span<const uint8_t> data, size_t at, bool bigEndian)
        {
            uint32_t v;
            memcpy(&v, data.data() + at, 4);
            return bigEndian ? ByteSwap(v) : v;
        }
    }

    const FieldDef* Template::Find(uint32_t label) const
    {
        const auto it = std::ranges::lower_bound(fields, label, {}, &FieldDef::label);
        return it != fields.end() && it->label == label ? &*it : nullptr;
    }

    Value* Value::Find(uint32_t label)
    {
        for (Field& field : fields)
            if (field.label == label)
                return &field.value;
        return nullptr;
    }

    const Value* Value::Find(uint32_t label) const
    {
        return const_cast<Value*>(this)->Find(label);
    }

    Value& Value::At(uint32_t label)
    {
        if (Value* value = Find(label))
            return *value;
        return fields.emplace_back(Field{ label, Value() }).value;
    }

    uint32_t Value::U32() const
    {
        if (kind != Kind::Prim || bytes.size() != 4)
            return 0;
        uint32_t v;
        memcpy(&v, bytes.data(), 4);
        return v;
    }

    void Value::SetU32(uint32_t value)
    {
        kind = Kind::Prim;
        if (type == kU8)   // (not set yet)
            type = kU32;
        bytes.resize(4);
        memcpy(bytes.data(), &value, 4);
    }

    uint32_t PrimSize(uint16_t type)
    {
        switch (type)
        {
        case kU8: case kI8: return 1;
        case kU16: case kI16: return 2;
        case kU32: case kI32: case kF32: return 4;
        case kU64: case kI64: case kF64: return 8;
        case kVec3: return 12;
        case kVec4: case kQuat: case kColor: return 16;
        case kMatrix: return 64;
        default: return 0;
        }
    }

    uint32_t SlotSize(uint16_t type, uint16_t flags, const std::vector<Template>& templates)
    {
        if (type == kGeneric)
            return 8;
        if (flags & (kList | kRef))
            return 4;
        if (flags & kStruct)
            return type < templates.size() ? templates[type].size : 0;
        if (type == kString)
            return 4;
        if (type == kTlk)
            return 8;
        return PrimSize(type);
    }

    uint32_t StructAlign(const Template& tpl, const std::vector<Template>& templates, int depth)
    {
        // The inline structs, nearest first, down to 16 deep: each template is looked at
        // once, at its least depth (not once per path to it, which grows exponentially).
        std::vector<bool> seen(templates.size());
        std::vector<const Template*> level = { &tpl }, next;
        for (; depth <= 16 && !level.empty(); depth++, level.swap(next))
        {
            next.clear();
            for (const Template* t : level)
            {
                for (const FieldDef& field : t->fields)
                {
                    if ((field.flags & (kList | kRef)) || field.type == kGeneric)
                        continue;
                    if (field.flags & kStruct)
                    {
                        if (field.type < templates.size() && !seen[field.type])
                        {
                            seen[field.type] = true;
                            next.push_back(&templates[field.type]);
                        }
                    }
                    else if (field.type == kVec4 || field.type == kQuat || field.type == kColor ||
                        field.type == kMatrix)
                        return 16;
                }
            }
        }
        return 4;
    }

    std::optional<std::vector<Template>> ReadTemplates(std::span<const uint8_t> data)
    {
        if (data.size() < kHeaderSize || memcmp(data.data(), "GFF V4.0", 8) != 0)
            return std::nullopt;
        const bool bigEndian = memcmp(data.data() + 8, "X360", 4) == 0;
        const uint32_t count = Get32(data, 20, bigEndian);
        if (count == 0 || count > (data.size() - kHeaderSize) / kStructEntrySize)
            return std::nullopt;
        std::vector<Template> templates(count);
        // All the templates' fields together, at most as many as the file has room for: each
        // table is checked alone, and many templates naming one big table would otherwise
        // copy it again for each (gigabytes from a megabyte).
        size_t totalFields = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const size_t at = kHeaderSize + i * kStructEntrySize;
            Template& tpl = templates[i];
            memcpy(tpl.fourcc.data(), data.data() + at, 4);
            const uint32_t fieldCount = Get32(data, at + 4, bigEndian);
            const uint32_t fieldTable = Get32(data, at + 8, bigEndian);
            tpl.size = Get32(data, at + 12, bigEndian);
            if (fieldCount == 0)
                continue;
            if (fieldTable > data.size() || fieldCount > (data.size() - fieldTable) / kFieldEntrySize)
                return std::nullopt;
            totalFields += fieldCount;
            if (totalFields > data.size() / kFieldEntrySize)
                return std::nullopt;
            tpl.fields.reserve(fieldCount);
            for (uint32_t k = 0; k < fieldCount; ++k)
            {
                const size_t f = fieldTable + size_t(k) * kFieldEntrySize;
                const uint32_t typeFlags = Get32(data, f + 4, bigEndian);
                tpl.fields.push_back({ Get32(data, f, bigEndian), uint16_t(typeFlags), uint16_t(typeFlags >> 16),
                    Get32(data, f + 8, bigEndian) });
            }
            std::ranges::sort(tpl.fields, {}, &FieldDef::label);
        }
        return templates;
    }

    std::optional<File> Read(std::span<const uint8_t> data)
    {
        auto templates = ReadTemplates(data);
        if (!templates)
            return std::nullopt;
        File file;
        memcpy(file.platform.data(), data.data() + 8, 4);
        memcpy(file.type.data(), data.data() + 12, 4);
        memcpy(file.version.data(), data.data() + 16, 4);
        file.templates = std::move(*templates);
        const bool bigEndian = file.IsX360();
        try
        {
            Reader reader(data, bigEndian, Get32(data, 24, bigEndian), file.templates);
            file.root = reader.Read(0, kStruct, Get32(data, 24, bigEndian), 0);
        }
        catch (const Malformed&)
        {
            return std::nullopt;
        }
        return file;
    }

    std::vector<uint8_t> WriteX360(const File& file, const std::vector<Template>& templates)
    {
        Writer writer(templates);
        const Template& root = templates.at(0);
        const size_t base = writer.Alloc(root.size, StructAlign(root, templates));
        writer.Fill(0, kStruct, file.root, base, 0);

        std::vector<uint8_t> out;
        auto append = [&](const void* data, size_t size) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            out.insert(out.end(), bytes, bytes + size);
        };
        auto append32 = [&](uint32_t v) {
            v = ByteSwap(v);
            append(&v, 4);
        };
        append("GFF V4.0X360", 12);
        append(file.type.data(), 4);
        append(file.version.data(), 4);
        append32(uint32_t(templates.size()));
        append32(0);   // the data offset, patched below
        size_t fieldTable = kHeaderSize + kStructEntrySize * templates.size();
        for (const Template& tpl : templates)
        {
            append(tpl.fourcc.data(), 4);
            append32(uint32_t(tpl.fields.size()));
            append32(tpl.fields.empty() ? kNull : uint32_t(fieldTable));
            append32(tpl.size);
            fieldTable += kFieldEntrySize * tpl.fields.size();
        }
        for (const Template& tpl : templates)
            for (const FieldDef& field : tpl.fields)
            {
                append32(field.label);
                append32(field.type | uint32_t(field.flags) << 16);
                append32(field.offset);
            }
        while (out.size() % 16)
            out.push_back(0xFF);
        const uint32_t dataOffset = ByteSwap(uint32_t(out.size()));
        memcpy(out.data() + 24, &dataOffset, 4);
        out.insert(out.end(), writer.out.begin(), writer.out.end());
        return out;
    }
}
