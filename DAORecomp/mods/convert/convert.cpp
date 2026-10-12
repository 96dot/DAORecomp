#include "stdafx.h"
#include "mods/convert/convert.h"

#include <fstream>
#include <set>
#include <variant>

#include "files.h"
#include "mods/convert/gff.h"
#include "mods/convert/mesh.h"
#include "mods/convert/texture.h"
#include "mods/game_files.h"
#include "mods/mod_files.h"
#include "mods/mod_install.h"
#include "mods/mods.h"
#include "text.h"
#include "ui/erf.h"

#include "third_party/stb/stb_image.h"

namespace dao::mods
{
    namespace fs = std::filesystem;
    using namespace convert;

    namespace
    {
        // Bigger files aren't mod files the game could load anyway.
        constexpr uintmax_t kMaxFileBytes = 256u << 20;
        constexpr uint32_t kMaxGameFileBytes = 64u << 20;
        // All a conversion reads (unpacked): more than a mod the game could hold, so
        // a damaged or hostile archive can't keep it busy for hours.
        constexpr uint64_t kMaxReadBytes = 4ull << 30;
        // Converting stops when the drive gets this full.
        constexpr uintmax_t kMinFreeBytes = 1ull << 30;
        // Game files read for one file type's struct layouts without finding a new
        // one: then the rest are laid out as the game's own are (see Catalog::For).
        constexpr int kMaxFruitlessGameFiles = 300;
        // Where conversions work (under mods/, hidden): a folder each, holding the PC
        // packages unpacked and the converted files until they're put in place.
        constexpr char kWorkFolder[] = ".converting";
        static_assert(kWorkFolder[0] == kHiddenPrefix && kOriginalsFolder[0] == kHiddenPrefix);

        std::string FourccText(const gff::Fourcc& f)
        {
            return std::string(f.data(), 4);
        }

        std::string ExactKey(const gff::Fourcc& type, const gff::Template& tpl)
        {
            std::string key = FourccText(type) + FourccText(tpl.fourcc);
            for (const gff::FieldDef& field : tpl.fields)
                key += fmt::format(",{}", field.label);
            return key;
        }

        // The game's struct layouts per (file type, fourcc, field labels), and per
        // (file type, fourcc) for structs with optional fields. Struct-typed fields
        // name the struct by fourcc, since struct ids differ between files.
        class Catalog
        {
        public:
            void Add(std::span<const uint8_t> data)
            {
                const auto templates = gff::ReadTemplates(data);
                if (!templates || memcmp(data.data() + 8, "X360", 4) != 0)
                    return;
                gff::Fourcc type;
                memcpy(type.data(), data.data() + 12, 4);
                for (const gff::Template& tpl : *templates)
                {
                    Known known{ tpl, {} };
                    for (const gff::FieldDef& field : tpl.fields)
                        known.refs.push_back((field.flags & gff::kStruct) && field.type < templates->size()
                                ? std::optional((*templates)[field.type].fourcc)
                                : std::nullopt);
                    exact_.emplace(ExactKey(type, tpl), known);
                    any_.emplace(FourccText(type) + FourccText(tpl.fourcc), std::move(known));
                }
            }

            bool Has(const std::string& exactKey) const { return exact_.contains(exactKey); }

            // Whether For has the game's layout for a struct (not one derived).
            bool Covers(const gff::Fourcc& type, const gff::Template& src) const
            {
                if (exact_.contains(ExactKey(type, src)))
                    return true;
                const auto it = any_.find(FourccText(type) + FourccText(src.fourcc));
                return it != any_.end() && std::ranges::all_of(src.fields,
                    [&](const gff::FieldDef& f) { return it->second.tpl.Find(f.label) != nullptr; });
            }

            // The 360 layout for PC struct `src`, its struct fields renumbered to the PC file's struct ids.
            std::variant<gff::Template, std::string> For(const gff::Fourcc& type, const gff::Template& src,
                const std::vector<gff::Template>& srcTemplates) const
            {
                if (!Covers(type, src))
                    return Derive(src, srcTemplates);
                auto it = exact_.find(ExactKey(type, src));
                if (it == exact_.end())
                    it = any_.find(FourccText(type) + FourccText(src.fourcc));
                const Known& known = it->second;
                gff::Template out{ known.tpl.fourcc, known.tpl.size, {} };
                for (size_t i = 0; i < known.tpl.fields.size(); ++i)
                {
                    gff::FieldDef field = known.tpl.fields[i];
                    if (const gff::FieldDef* pc = src.Find(field.label))
                    {
                        const bool sameType = (pc->flags & gff::kStruct)
                            ? pc->type < srcTemplates.size() && known.refs[i] == srcTemplates[pc->type].fourcc
                            : pc->type == field.type;
                        if (!sameType || pc->flags != field.flags)
                            return fmt::format("struct {} field {} has another type than the game's",
                                FourccText(src.fourcc), field.label);
                        field.type = pc->type;
                    }
                    else if (field.flags & gff::kStruct)
                        return fmt::format("struct {} lacks field {}, which the game's has", FourccText(src.fourcc),
                            field.label);
                    out.fields.push_back(field);
                }
                return out;
            }

        private:
            struct Known
            {
                gff::Template tpl;
                std::vector<std::optional<gff::Fourcc>> refs;   // per field
            };

            // A layout for a struct the game has no example of, the way its own are
            // laid out: the widest alignment first, otherwise in the PC's order.
            static gff::Template Derive(const gff::Template& src, const std::vector<gff::Template>& templates)
            {
                auto align = [&](const gff::FieldDef& f) -> uint32_t {
                    if ((f.flags & (gff::kList | gff::kRef)) || f.type == gff::kString || f.type == gff::kTlk ||
                        f.type == gff::kGeneric)
                        return 4;
                    if (f.flags & gff::kStruct)
                        return f.type < templates.size() ? gff::StructAlign(templates[f.type], templates) : 4;
                    const uint32_t size = gff::PrimSize(f.type);
                    return size == 16 || size == 64 ? 16 : std::min(std::max(size, 1u), 8u);
                };
                // (Alignment, field), each alignment worked out once.
                std::vector<std::pair<uint32_t, gff::FieldDef>> order;
                order.reserve(src.fields.size());
                for (const gff::FieldDef& f : src.fields)
                    order.emplace_back(align(f), f);
                std::ranges::stable_sort(order, [](const auto& a, const auto& b) {
                    return a.first != b.first ? a.first > b.first : a.second.offset < b.second.offset;
                });
                gff::Template out{ src.fourcc, 0, {} };
                uint32_t at = 0, most = 1;
                for (auto [a, field] : order)
                {
                    most = std::max(most, a);
                    at = (at + a - 1) / a * a;
                    field.offset = at;
                    at += gff::SlotSize(field.type, field.flags, templates);
                    out.fields.push_back(field);
                }
                out.size = (at + most - 1) / most * most;
                std::ranges::sort(out.fields, {}, &gff::FieldDef::label);
                return out;
            }

            std::unordered_map<std::string, Known> exact_;
            std::unordered_map<std::string, Known> any_;
        };

        std::vector<uint8_t> ReadHead(const fs::path& path)
        {
            return mods::ReadHead(path, kFormatHeadBytes);
        }

        // A PC archive: ERF V2.0, or V2.2 (whose files may be compressed), with UTF-16 names.
        class PcArchive
        {
        public:
            struct Entry
            {
                std::string name;
                uint32_t offset, packedSize, size;
            };

            static std::variant<PcArchive, std::string> Open(const fs::path& path)
            {
                constexpr size_t kHeaderSize = 56;
                constexpr uint32_t kMaxEntries = 1u << 16;
                constexpr size_t kNameChars = 32;
                std::error_code ec;
                const uint64_t fileSize = fs::file_size(path, ec);
                if (ec)
                    return std::string("it can't be read");
                const auto header = ui::ReadFileBytes(path, 0, uint32_t(std::min<uint64_t>(fileSize, kHeaderSize)));
                if (!header || header->size() < 32)
                    return std::string("it's damaged");
                std::string magic;
                for (size_t i = 0; i < 8; ++i)
                    magic += (*header)[2 * i + 1] == 0 ? char((*header)[2 * i]) : '?';
                if (magic == "ERF V3.0")
                    return std::string("it's made for Dragon Age II");
                const bool v22 = magic == "ERF V2.2";
                if (magic != "ERF V2.0" && !v22)
                    return std::string("it isn't an archive this can read");
                PcArchive archive;
                archive.path_ = path;
                archive.fileSize_ = fileSize;
                const uint32_t count = ui::ReadLE32(*header, 16);
                if (v22)
                {
                    if (header->size() < kHeaderSize)
                        return std::string("it's damaged");
                    const uint32_t flags = ui::ReadLE32(*header, 32);
                    if ((flags >> 4) & 0xF)
                        return std::string("it's encrypted (downloadable content)");
                    archive.compressed_ = (flags >> 29) != 0;
                }
                const size_t entriesAt = v22 ? 56 : 32, entrySize = v22 ? 76 : 72;
                if (count > kMaxEntries)
                    return std::string("it's damaged");
                const auto table = ui::ReadFileBytes(path, entriesAt, uint32_t(count * entrySize));
                if (!table)
                    return std::string("it's damaged");
                for (uint32_t i = 0; i < count; ++i)
                {
                    const std::span<const uint8_t> raw(table->data() + i * entrySize, entrySize);
                    std::wstring name;
                    for (size_t c = 0; c < kNameChars && (raw[2 * c] || raw[2 * c + 1]); ++c)
                        name += wchar_t(raw[2 * c] | raw[2 * c + 1] << 8);
                    Entry entry{ WideToUtf8(name), ui::ReadLE32(raw, 64), ui::ReadLE32(raw, 68), 0 };
                    entry.size = v22 ? ui::ReadLE32(raw, 72) : entry.packedSize;
                    if (!IsPlainName(entry.name))
                        return fmt::format("a file in it has a name a file can't have ({})", entry.name);
                    if (uint64_t(entry.offset) + entry.packedSize > fileSize)
                        return std::string("it's damaged");
                    archive.entries_.push_back(std::move(entry));
                }
                // Files don't share data (else a small archive could unpack to a lot).
                std::vector<std::pair<uint64_t, uint64_t>> spans;   // (offset, end)
                for (const Entry& entry : archive.entries_)
                    if (entry.packedSize)
                        spans.emplace_back(entry.offset, uint64_t(entry.offset) + entry.packedSize);
                std::ranges::sort(spans);
                for (size_t i = 1; i < spans.size(); ++i)
                    if (spans[i].first < spans[i - 1].second)
                        return std::string("it's damaged (its files overlap)");
                return archive;
            }

            const std::vector<Entry>& Entries() const { return entries_; }

            // An entry's data (unpacked); nothing if it's damaged or too big.
            std::optional<std::vector<uint8_t>> Read(const Entry& entry) const
            {
                if (entry.size > kMaxFileBytes || entry.packedSize > kMaxFileBytes)
                    return std::nullopt;
                auto data = ui::ReadFileBytes(path_, entry.offset, entry.packedSize);
                if (!data || !compressed_ || entry.size == 0)
                    return data;
                // Deflate packs at most about 1032 to 1: more is damage (and an
                // allocation for nothing).
                if (entry.packedSize == 0 || entry.size / entry.packedSize > 1032)
                    return std::nullopt;
                // BioWare's zlib (a byte, then a raw deflate stream), a raw stream or a
                // zlib one, whichever unpacks to the size.
                std::vector<uint8_t> out(entry.size);
                const auto* in = reinterpret_cast<const char*>(data->data());
                auto* dst = reinterpret_cast<char*>(out.data());
                const int size = int(entry.size), packed = int(data->size());
                if (stbi_zlib_decode_buffer(dst, size, in, packed) == size ||
                    stbi_zlib_decode_noheader_buffer(dst, size, in, packed) == size ||
                    (packed > 1 && stbi_zlib_decode_noheader_buffer(dst, size, in + 1, packed - 1) == size))
                    return out;
                // Stored as it is.
                if (entry.packedSize == entry.size)
                    return data;
                return std::nullopt;
            }

        private:
            fs::path path_;
            uint64_t fileSize_ = 0;
            bool compressed_ = false;
            std::vector<Entry> entries_;
        };

        // Normal maps by name, for those no material in the mod names.
        bool LooksLikeNormalMap(std::string_view stem)
        {
            for (std::string_view end : { "_n", "0n", "_nm", "_nrm", "_norm", "_normal" })
                if (stem.ends_with(end))
                    return true;
            return false;
        }

        // A menu atlas's layout (atl_<name>_dat.xml): the pages (atl_<name>_00.dds, ...)
        // and where on them each of the menus' pictures is.
        bool IsAtlasLayout(std::string_view name)
        {
            return name.starts_with("atl_") && name.ends_with("_dat.xml");
        }

        // An atlas layout's places, without the pictures' names (which differ between
        // the versions): each one's page, place on it (in texels of a 4096 page, so
        // however it's written) and size.
        std::set<std::string> AtlasPlaces(std::string_view xml)
        {
            std::set<std::string> places;
            std::string page;
            // (Each element from its '<' to its '>', the next looked for after that.)
            size_t close = 0;
            for (size_t at = xml.find('<'); at != std::string_view::npos && (close = xml.find('>', at)) != std::string_view::npos;
                 at = xml.find('<', close))
            {
                const std::string_view element = xml.substr(at, close - at);
                if (element.starts_with("<AtlasTexture"))
                    page = Lower(XmlAttribute(element, "Name"));
                else if (element.starts_with("<SourceTexture"))
                {
                    const std::string place(XmlAttribute(element, "OffsetAndScale"));
                    float v[4];
                    if (sscanf(place.c_str(), "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]) != 4)
                        continue;
                    places.insert(fmt::format("{} {} {} {} {} {}", page, lround(v[0] * 4096), lround(v[1] * 4096),
                        lround(v[2] * 4096), lround(v[3] * 4096), XmlAttribute(element, "OriginalImageSize")));
                }
            }
            return places;
        }

        // The textures a material (.mao, XML) uses as normal maps: their names, lower
        // case, without the extension.
        void FindNormalMaps(std::string_view mao, std::set<std::string>& out)
        {
            size_t close = 0;
            for (size_t at = mao.find("<Texture"); at != std::string_view::npos && (close = mao.find('>', at)) != std::string_view::npos;
                 at = mao.find("<Texture", close))
            {
                const std::string_view element = mao.substr(at, close - at);
                if (Lower(XmlAttribute(element, "Name")).find("normal") == std::string::npos)
                    continue;
                const std::string texture = Lower(XmlAttribute(element, "ResName"));
                out.insert(texture.substr(0, texture.rfind('.')));
            }
        }

        enum class Kind
        {
            Texture,   // a .dds, converted to .xds
            Other,     // converted if it's a PC GFF file, else copied as it is
        };

        struct Job
        {
            Kind kind;
            fs::path source;      // the file, or the archive it's in
            int archive = -1;     // the archive's index, if it's in one
            size_t entry = 0;
            std::string label;    // where it is in the mod, for messages
            fs::path target;      // the 360 file
            TextureOptions texture;
            bool distant = false;   // a copy for distant models
            fs::path temp;          // converted, in the work folder
        };

        // Animation curves (CURV, in dialogs' and cutscenes' animations): the PC's have
        // their keyframes in a list of VERT structs (time, value) and the tangents
        // between them in a list of TRAN structs; the 360's have one float list: 0,
        // then each keyframe's time and value. (All 13,853 PC curves of a dialog mod,
        // and the 360's of the same dialogs, have straight tangents: TRAN type 1 and
        // no control points. The 0 is taken to mean that; other tangents become it.)
        constexpr gff::Fourcc kCurve = { 'C', 'U', 'R', 'V' };
        constexpr uint32_t kCurveKeys = 5351, kCurveTangents = 5352, kCurveFloats = 5354;
        constexpr uint32_t kKeyTime = 5370, kKeyValue = 5371, kTangentType = 5380;
        constexpr uint8_t kStraightTangent = 1;

        // Gives PC curve templates the 360's fields; their struct ids.
        std::set<uint32_t> ConvertCurveTemplates(std::vector<gff::Template>& templates)
        {
            std::set<uint32_t> curves;
            for (uint32_t sid = 0; sid < templates.size(); ++sid)
            {
                gff::Template& tpl = templates[sid];
                if (tpl.fourcc != kCurve || !tpl.Find(kCurveKeys) || tpl.Find(kCurveFloats))
                    continue;
                std::erase_if(tpl.fields,
                    [](const gff::FieldDef& f) { return f.label == kCurveKeys || f.label == kCurveTangents; });
                tpl.fields.push_back({ kCurveFloats, gff::kF32, gff::kList, 0 });
                std::ranges::sort(tpl.fields, {}, &gff::FieldDef::label);
                curves.insert(sid);
            }
            return curves;
        }

        // Converts the file's PC curves to the 360's form; the number whose tangents
        // weren't straight (and became so).
        size_t ConvertCurves(gff::File& file)
        {
            const std::set<uint32_t> curves = ConvertCurveTemplates(file.templates);
            if (curves.empty())
                return 0;
            auto f32 = [](float value) {
                gff::Value v;
                v.kind = gff::Value::Kind::Prim;
                v.type = gff::kF32;
                v.bytes.resize(sizeof(value));
                memcpy(v.bytes.data(), &value, sizeof(value));   // little-endian, as Read gives them
                return v;
            };
            size_t bent = 0;
            std::vector<gff::Value*> stack = { &file.root };
            while (!stack.empty())
            {
                gff::Value& v = *stack.back();
                stack.pop_back();
                for (gff::Value& item : v.items)
                    stack.push_back(&item);
                for (gff::Field& field : v.fields)
                    stack.push_back(&field.value);
                if (v.kind != gff::Value::Kind::Struct || !curves.contains(v.sid))
                    continue;
                gff::Value floats;
                if (const gff::Value* keys = v.Find(kCurveKeys); keys && keys->kind == gff::Value::Kind::List)
                {
                    floats.kind = gff::Value::Kind::List;
                    floats.type = gff::kF32;
                    floats.items.push_back(f32(0.0f));
                    for (const gff::Value& key : keys->items)
                    {
                        for (const uint32_t label : { kKeyTime, kKeyValue })
                        {
                            const gff::Value* part = key.Find(label);
                            floats.items.push_back(part && part->kind == gff::Value::Kind::Prim &&
                                    part->bytes.size() == sizeof(float)
                                ? *part
                                : f32(0.0f));
                        }
                    }
                }
                if (const gff::Value* tangents = v.Find(kCurveTangents))
                {
                    const bool straight = std::ranges::all_of(tangents->items, [](const gff::Value& tangent) {
                        const gff::Value* type = tangent.Find(kTangentType);
                        return type && type->kind == gff::Value::Kind::Prim && type->bytes.size() == 1 &&
                            type->bytes[0] == kStraightTangent;
                    });
                    bent += !straight;
                }
                std::erase_if(v.fields,
                    [](const gff::Field& f) { return f.label == kCurveKeys || f.label == kCurveTangents; });
                v.fields.push_back({ kCurveFloats, std::move(floats) });
            }
            return bent;
        }

        std::variant<std::vector<uint8_t>, std::string> ConvertGff(std::span<const uint8_t> data,
            const Catalog& catalog, const std::string& label)
        {
            try
            {
                auto file = gff::Read(data);
                if (!file)
                    return std::string("it isn't a well-formed GFF file");
                if (const size_t bent = ConvertCurves(*file))
                    LOGW("Convert: {}: {} animation curves had tangents other than straight ones, which they now have",
                        label, bent);
                if (file->type == gff::Fourcc{ 'M', 'E', 'S', 'H' })
                    if (auto error = ConvertMesh(*file))
                        return *error;
                std::vector<gff::Template> templates;
                for (const gff::Template& tpl : file->templates)
                {
                    auto layout = catalog.For(file->type, tpl, file->templates);
                    if (auto* error = std::get_if<std::string>(&layout))
                        return *error;
                    templates.push_back(std::move(std::get<gff::Template>(layout)));
                }
                return gff::WriteX360(*file, templates);
            }
            catch (const std::bad_alloc&)
            {
                return std::string("it's too big to convert");
            }
            catch (const std::exception& error)
            {
                return std::string(error.what());
            }
        }

        bool WriteAll(const fs::path& path, std::span<const uint8_t> data)
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            return file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size())) && file.flush();
        }

        // mods/.originals/<mod> (converting a mod again adds to it).
        fs::path OriginalsFolder(const fs::path& modFolder)
        {
            return modFolder.parent_path() / kOriginalsFolder / modFolder.filename();
        }

        std::string Relative(const fs::path& path, const fs::path& base)
        {
            return WideToUtf8(path.lexically_relative(base).generic_wstring());
        }

        // One mod's conversion: what to convert (jobs), the PC files to set aside,
        // and the outcome.
        class Conversion
        {
        public:
            Conversion(const fs::path& modFolder, const fs::path& gameRoot, const GameFiles& game,
                std::stop_token stop, ConvertResult& result)
                : mod_(modFolder), gameRoot_(gameRoot), game_(game), stop_(stop), result_(result),
                  work_(modFolder.parent_path() / kWorkFolder /
                      (modFolder.filename().native() + L"-" + std::to_wstring(GetCurrentProcessId())))
            {
            }

            void Run()
            {
                Gather();
                if (!result_.problems.empty() || Stopped())
                    return;
                FixAtlases();
                if (Stopped())
                    return;
                if (jobs_.empty() && aside_.empty())
                {
                    result_.problems.push_back("It has no files in the PC version's formats.");
                    return;
                }
                MarkNormalMaps();
                AddDistantCopies();
                if (Stopped() || !ConvertAll())
                    return;
                PutInPlace();
                if (result_.converted && manifest_)
                    DescribeFromManifest(mod_, *manifest_);
            }

        private:
            // Whether to stop (said once).
            bool Stopped()
            {
                if (stop_.stop_requested() && !stopped_)
                {
                    stopped_ = true;
                    result_.problems.push_back("Stopped.");
                }
                return stopped_;
            }

            void Gather()
            {
                std::vector<fs::path> files;
                std::error_code ec;
                for (auto it = fs::recursive_directory_iterator(mod_, ec);
                     !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                {
                    if (std::error_code fileEc; it->is_regular_file(fileEc))
                        files.push_back(it->path());
                }
                if (ec)
                {
                    result_.problems.push_back(fmt::format("Can't list the mod's files: {}", ec.message()));
                    return;
                }
                // Loose files first, so they win over archives' and packages' of the same name.
                std::vector<fs::path> archives, packages;
                for (const fs::path& path : files)
                {
                    if (Stopped())
                        return;
                    const fs::path relative = path.lexically_relative(mod_);
                    const std::string extension = Lower(Utf8(path.extension()));
                    // (Only those that may be PC files are read.)
                    const auto head = extension == ".xds" || IsDocument(path) ? std::vector<uint8_t>() : ReadHead(path);
                    if (IsInAddinModule(relative))
                    {
                        // An installed package's module: of no use here.
                        LeaveOutModuleFile(path);
                        aside_.push_back(path);
                    }
                    else if (extension == ".dazip")
                        packages.push_back(path);
                    else if (IsPcArchive(head))
                        archives.push_back(path);
                    else if (extension == ".dds")
                    {
                        fs::path target = path;
                        target.replace_extension(".xds");
                        aside_.push_back(path);
                        // A mod with both keeps its own .xds.
                        if (!fs::exists(target, ec))
                            AddJob({ Kind::Texture, path, -1, 0, Relative(path, mod_), target });
                    }
                    else if (IsPcGff(head))
                    {
                        aside_.push_back(path);
                        AddJob({ Kind::Other, path, -1, 0, Relative(path, mod_), path });
                    }
                    else if (extension == ".mao")
                        materials_.push_back(path);
                    else if (IsAtlasLayout(Lower(Utf8(path.filename()))))
                        atlases_.push_back(path);
                }
                for (const fs::path& path : archives)
                {
                    aside_.push_back(path);
                    // In override/, its files go next to it; elsewhere, into override/<its name>/.
                    const fs::path into = IsUnder(path.lexically_relative(mod_).parent_path(), "override")
                        ? path.parent_path()
                        : mod_ / "packages" / "core" / "override" / path.stem();
                    AddArchive(path, into, Relative(path, mod_));
                }
                for (const fs::path& path : packages)
                {
                    if (Stopped())
                        return;
                    aside_.push_back(path);
                    const fs::path into = IsUnder(path.lexically_relative(mod_).parent_path(), "override")
                        ? path.parent_path() / path.stem()
                        : mod_ / "packages" / "core" / "override" / path.stem();
                    AddPackage(path, into);
                }
                if (moduleContent_)
                    result_.notes.push_back("Its add-in module (new areas or a campaign) was left out: the Xbox 360 "
                                            "version can't load add-in modules, so only its other files are used.");
                else if (moduleText_)
                    result_.notes.push_back("Its add-in's talk table was left out (the add-in's name and description "
                                            "for the PC version's list of add-ins): only add-ins can use one.");
            }

            void AddJob(Job job)
            {
                const std::string key = Lower(Utf8(job.target));
                if (const auto it = targets_.find(key); it != targets_.end())
                {
                    result_.notes.push_back(fmt::format("{} and {} both make {}: kept the first.",
                        jobs_[it->second].label, job.label, Relative(job.target, mod_)));
                    return;
                }
                std::error_code ec;
                // A file already there that this doesn't replace (one set aside is replaced).
                if (job.target != job.source && fs::exists(job.target, ec) &&
                    std::ranges::find(aside_, job.target) == aside_.end())
                {
                    result_.notes.push_back(fmt::format("Kept the mod's own {} (not the one from {}).",
                        Relative(job.target, mod_), job.label));
                    return;
                }
                targets_.emplace(key, jobs_.size());
                jobs_.push_back(std::move(job));
            }

            void AddArchive(const fs::path& path, const fs::path& into, const std::string& label)
            {
                auto opened = PcArchive::Open(path);
                if (auto* error = std::get_if<std::string>(&opened))
                {
                    result_.leftOut.push_back(fmt::format("{}: {}", label, *error));
                    return;
                }
                archives_.push_back(std::move(std::get<PcArchive>(opened)));
                const int index = int(archives_.size() - 1);
                const auto& entries = archives_.back().Entries();
                for (size_t i = 0; i < entries.size(); ++i)
                {
                    fs::path target = into / FromUtf8(entries[i].name);
                    const bool texture = Lower(Utf8(target.extension())) == ".dds";
                    if (texture)
                        target.replace_extension(".xds");
                    AddJob({ texture ? Kind::Texture : Kind::Other, path, index, i,
                        label + "/" + entries[i].name, target });
                }
            }

            // A file of an add-in's module, which is left out: whether it's only text
            // (talk tables, or an archive of them) or more.
            void LeaveOutModuleFile(const fs::path& path)
            {
                bool text = Lower(Utf8(path.extension())) == ".tlk";
                if (!text && IsPcArchive(ReadHead(path)))
                {
                    auto opened = PcArchive::Open(path);
                    if (const auto* archive = std::get_if<PcArchive>(&opened))
                        text = std::ranges::all_of(archive->Entries(),
                            [](const auto& entry) { return Lower(entry.name).ends_with(".tlk"); });
                }
                (text ? moduleText_ : moduleContent_) = true;
            }

            // Unpacks a .dazip and adds its files (but its add-in modules).
            void AddPackage(const fs::path& path, const fs::path& into)
            {
                const std::string label = Relative(path, mod_);
                const fs::path unpacked = work_.Dir() / L"packages" / std::to_wstring(++unpacked_);
                std::error_code ec;
                fs::create_directories(unpacked, ec);
                std::string error;
                if (ec || !UnpackArchive(path, unpacked, stop_, error))
                {
                    result_.leftOut.push_back(fmt::format("{}: {}", label, ec ? ec.message() : error));
                    return;
                }
                // Its files are in Contents/; Manifest.xml beside it describes it (the
                // first package's describes the mod).
                const fs::path contents = unpacked / "Contents";
                if (!manifest_ && fs::is_regular_file(unpacked / "Manifest.xml", ec))
                    manifest_ = unpacked / "Manifest.xml";
                std::vector<fs::path> archives;
                for (auto it = fs::recursive_directory_iterator(contents, ec);
                     !ec && it != fs::recursive_directory_iterator() && !Stopped(); it.increment(ec))
                {
                    if (std::error_code fileEc; !it->is_regular_file(fileEc))
                        continue;
                    const fs::path relative = it->path().lexically_relative(contents);
                    if (IsInAddinModule(relative))
                    {
                        LeaveOutModuleFile(it->path());
                        continue;
                    }
                    const std::string entryLabel = label + "/" + WideToUtf8(relative.generic_wstring());
                    fs::path target = into / relative;
                    if (IsPcArchive(ReadHead(it->path())))
                        archives.push_back(it->path());
                    else if (Lower(Utf8(target.extension())) == ".dds")
                    {
                        target.replace_extension(".xds");
                        AddJob({ Kind::Texture, it->path(), -1, 0, entryLabel, target });
                    }
                    else
                        AddJob({ Kind::Other, it->path(), -1, 0, entryLabel, target });
                }
                for (const fs::path& archive : archives)
                {
                    const fs::path relative = archive.lexically_relative(contents);
                    AddArchive(archive, (into / relative).parent_path(),
                        label + "/" + WideToUtf8(relative.generic_wstring()));
                }
            }

            // What's read counts toward kMaxReadBytes; past it, nothing more is.
            std::optional<std::vector<uint8_t>> Counted(std::optional<std::vector<uint8_t>> data)
            {
                if (data && (readBytes_ += data->size()) > kMaxReadBytes)
                {
                    tooMuch_ = true;
                    return std::nullopt;
                }
                return data;
            }

            // A job's file (unpacked).
            std::optional<std::vector<uint8_t>> Load(const Job& job)
            {
                if (tooMuch_)
                    return std::nullopt;
                return Counted(job.archive >= 0
                        ? archives_[job.archive].Read(archives_[job.archive].Entries()[job.entry])
                        : ReadAll(job.source, kMaxFileBytes));
            }

            // Enough of a job's PC GFF file for its struct templates: a loose file's
            // are before its data, so the rest isn't read.
            std::optional<std::vector<uint8_t>> LoadTemplates(const Job& job)
            {
                constexpr size_t kGffHeaderBytes = 28, kDataOffsetAt = 24;
                if (job.archive >= 0)
                    return Load(job);
                const auto header = mods::ReadHead(job.source, kGffHeaderBytes);
                std::error_code ec;
                const uintmax_t size = fs::file_size(job.source, ec);
                if (header.size() < kGffHeaderBytes || !IsPcGff(header) || ec || tooMuch_)
                    return std::nullopt;
                const uintmax_t templatesEnd = std::min<uintmax_t>(ui::ReadLE32(header, kDataOffsetAt), size);
                return Counted(mods::ReadHead(job.source, size_t(std::min<uintmax_t>(templatesEnd, kMaxFileBytes))));
            }

            // Menu atlases replacing the game's: the 360 version's menus name their pictures
            // differently, and use some the PC's don't have, so a PC layout fails them.
            // Where the mod's pages have every picture of the game's layout in its place,
            // the game's layout is used with them; otherwise the atlas is left out.
            void FixAtlases()
            {
                constexpr uint32_t kMaxLayoutBytes = 1 << 20;
                struct Layout
                {
                    std::string name, label;
                    std::optional<std::vector<uint8_t>> data;
                    std::optional<fs::path> loose;   // or it's a job's
                };
                std::vector<Layout> layouts;
                for (const fs::path& path : atlases_)
                    layouts.push_back({ Lower(Utf8(path.filename())), Relative(path, mod_),
                        ReadAll(path, kMaxLayoutBytes), path });
                for (const Job& job : jobs_)
                {
                    if (const std::string name = Lower(Utf8(job.target.filename())); IsAtlasLayout(name))
                        layouts.push_back({ name, job.label, Load(job), std::nullopt });
                }
                std::set<std::string> drop;   // the lower-case names (without extensions) of jobs' targets
                for (const Layout& layout : layouts)
                {
                    if (Stopped())
                        return;
                    const auto own = game_.ReadFile(layout.name, kMaxLayoutBytes);
                    if (!own || !layout.data)
                        continue;   // A new atlas, whose layout is the mod's own.
                    const auto text = [](const std::vector<uint8_t>& data) {
                        return std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
                    };
                    const std::string atlas = layout.name.substr(0, layout.name.size() - strlen("_dat.xml"));
                    const auto places = AtlasPlaces(text(*layout.data));
                    // The mod's layout goes, either way.
                    if (layout.loose)
                        aside_.push_back(*layout.loose);
                    drop.insert(atlas + "_dat");
                    if (std::ranges::includes(places, AtlasPlaces(text(*own))))
                    {
                        result_.notes.push_back(fmt::format("Its menu atlas {} uses the Xbox 360 version's layout, "
                                                            "whose pictures are in the same places.",
                            atlas));
                        continue;
                    }
                    result_.leftOut.push_back(fmt::format("{}: its menu atlas ({}) is laid out differently from the "
                                                          "Xbox 360 version's, whose menus would fail with it",
                        layout.label, atlas));
                    // Its pages too.
                    for (const Job& job : jobs_)
                    {
                        const std::string stem = Lower(Utf8(job.target.stem()));
                        if (job.kind == Kind::Texture && stem.starts_with(atlas + "_"))
                            drop.insert(stem);
                    }
                }
                if (drop.empty())
                    return;
                std::erase_if(jobs_, [&](const Job& job) { return drop.contains(Lower(Utf8(job.target.stem()))); });
                targets_.clear();
                for (size_t i = 0; i < jobs_.size(); ++i)
                    targets_.emplace(Lower(Utf8(jobs_[i].target)), i);
            }

            // Textures the mod's materials use as normal maps (or named like one).
            void MarkNormalMaps()
            {
                std::set<std::string> normalMaps;
                auto find = [&](const std::optional<std::vector<uint8_t>>& data) {
                    if (data)
                        FindNormalMaps(std::string_view(reinterpret_cast<const char*>(data->data()), data->size()),
                            normalMaps);
                };
                for (const Job& job : jobs_)
                {
                    if (Lower(Utf8(job.target.extension())) == ".mao" && !Stopped())
                        find(Load(job));
                }
                for (const fs::path& material : materials_)
                {
                    if (!Stopped())
                        find(Counted(ReadAll(material, kMaxFileBytes)));
                }
                for (Job& job : jobs_)
                {
                    const std::string stem = Lower(Utf8(job.target.stem()));
                    job.texture.normalMap =
                        job.kind == Kind::Texture && (normalMaps.contains(stem) || LooksLikeNormalMap(stem));
                }
            }

            // Where the mod replaces a game file the 360 version has smaller copies of
            // (for distant models), the same copies of the mod's.
            void AddDistantCopies()
            {
                // The mod's own materials (XML, the same on both) are copied as they are.
                const size_t count = jobs_.size();
                std::vector<Job> materials;
                for (const fs::path& material : materials_)
                    materials.push_back({ Kind::Other, material, -1, 0, Relative(material, mod_), material });
                size_t added = 0;
                for (size_t i = 0; i < count + materials.size(); ++i)
                {
                    const Job base = i < count ? jobs_[i] : materials[i - count];
                    const std::string name = Lower(Utf8(base.target.filename()));
                    if (!game_.Has(name))
                        continue;
                    const std::string stem = Lower(Utf8(base.target.stem()));
                    const std::string extension = Lower(Utf8(base.target.extension()));
                    std::vector<std::pair<std::string, uint32_t>> copies;   // name, mips left out
                    if ((extension == ".mmh" || extension == ".phy") && stem.ends_with("_0"))
                    {
                        for (char level : { '1', '2', '3' })
                            copies.push_back({ stem.substr(0, stem.size() - 1) + level + extension, 0 });
                    }
                    else if (extension == ".mao")
                    {
                        copies.push_back({ stem + "_l2.mao", 0 });
                        copies.push_back({ stem + "_l3.mao", 0 });
                    }
                    else if (base.kind == Kind::Texture)
                    {
                        copies.push_back({ stem + "l2.xds", 1 });
                        copies.push_back({ stem + "l3.xds", 2 });
                    }
                    for (const auto& [copyName, drop] : copies)
                    {
                        if (!game_.Has(copyName))
                            continue;
                        Job copy = base;
                        copy.target = copy.target.parent_path() / FromUtf8(copyName);
                        copy.texture.dropLevels = drop;
                        copy.distant = true;
                        const size_t before = jobs_.size();
                        AddJob(std::move(copy));
                        added += jobs_.size() - before;
                    }
                }
                if (added)
                    result_.notes.push_back(fmt::format("Made {} smaller cop{} of the game files it replaces for "
                                                        "distant models, as the Xbox 360 version has.",
                        added, added == 1 ? "y" : "ies"));
            }

            // Converts every job to a file in the work folder; false if none could be.
            bool ConvertAll()
            {
                const Catalog catalog = BuildCatalog();
                const fs::path out = work_.Dir() / L"out";
                std::error_code ec;
                fs::create_directories(out, ec);
                std::set<std::string> failed;   // labels, once each
                for (size_t i = 0; i < jobs_.size(); ++i)
                {
                    Job& job = jobs_[i];
                    if (Stopped())
                        return false;
                    if (failed.contains(job.label))
                        continue;
                    const auto data = Load(job);
                    if (tooMuch_)
                    {
                        result_.problems.push_back(fmt::format("It unpacks to more than {} GB, more than a mod "
                                                               "could hold: it may be damaged.",
                            kMaxReadBytes >> 30));
                        return false;
                    }
                    std::variant<std::vector<uint8_t>, std::string> converted;
                    if (!data)
                        converted = std::string("it can't be read");
                    else if (job.kind == Kind::Texture)
                        converted = ConvertTexture(*data, job.texture);
                    else if (IsPcGff(*data))
                        converted = ConvertGff(*data, catalog, job.label);
                    else
                        converted = *data;
                    if (auto* error = std::get_if<std::string>(&converted))
                    {
                        failed.insert(job.label);
                        result_.leftOut.push_back(fmt::format("{}: {}", job.label, *error));
                        continue;
                    }
                    const auto& bytes = std::get<std::vector<uint8_t>>(converted);
                    const uintmax_t free = fs::space(out, ec).available;
                    if (!ec && free < kMinFreeBytes + bytes.size())
                    {
                        result_.problems.push_back("The drive is nearly full.");
                        return false;
                    }
                    job.temp = out / std::to_wstring(i);
                    if (!WriteAll(job.temp, bytes))
                    {
                        result_.problems.push_back(fmt::format("Can't write {}.", Relative(job.target, mod_)));
                        return false;
                    }
                }
                if (!jobs_.empty() && std::ranges::all_of(jobs_, [](const Job& job) { return job.temp.empty(); }))
                {
                    result_.problems.push_back("None of its files could be converted.");
                    return false;
                }
                return true;
            }

            // The game's layouts for every struct of the mod's GFF files, from the game's
            // files with the same extensions: until every struct has an exact match, or
            // kMaxFruitlessGameFiles in a row add none.
            Catalog BuildCatalog()
            {
                std::set<std::string> extensions;
                std::set<std::string> wanted;
                for (const Job& job : jobs_)
                {
                    if (Stopped())
                        return {};
                    if (job.kind != Kind::Other || job.distant)
                        continue;
                    const auto data = LoadTemplates(job);
                    if (!data || !IsPcGff(*data))
                        continue;
                    auto templates = gff::ReadTemplates(*data);
                    if (!templates)
                        continue;
                    // As they'll be.
                    ConvertCurveTemplates(*templates);
                    extensions.insert(Lower(Utf8(job.target.extension())));
                    gff::Fourcc type;
                    memcpy(type.data(), data->data() + 12, 4);
                    for (const gff::Template& tpl : *templates)
                        wanted.insert(ExactKey(type, tpl));
                }
                Catalog catalog;
                if (extensions.empty())
                    return catalog;
                int fruitless = 0;
                std::error_code ec;
                for (const auto& archive : fs::directory_iterator(gameRoot_ / "packages" / "core" / "data", ec))
                {
                    if (Lower(Utf8(archive.path().extension())) != ".erf")
                        continue;
                    const auto erf = ui::ErfArchive::Open(archive.path());
                    if (!erf)
                        continue;
                    for (const auto& entry : erf->Entries())
                    {
                        if (Stopped() || wanted.empty() || fruitless >= kMaxFruitlessGameFiles)
                            return catalog;
                        const size_t dot = entry.name.rfind('.');
                        if (dot == std::string::npos || !extensions.contains(Lower(entry.name.substr(dot))))
                            continue;
                        const auto data = erf->Read(entry, kMaxGameFileBytes);
                        if (!data || data->size() < 28 || memcmp(data->data(), "GFF V4.0", 8) != 0)
                            continue;
                        catalog.Add(*data);
                        const size_t before = wanted.size();
                        std::erase_if(wanted, [&](const std::string& key) { return catalog.Has(key); });
                        fruitless = wanted.size() < before ? 0 : fruitless + 1;
                    }
                }
                return catalog;
            }

            // Sets the PC files aside and puts the converted ones in their place (or
            // nothing changes).
            void PutInPlace()
            {
                std::error_code ec;
                const fs::path originals = OriginalsFolder(mod_);
                std::vector<std::pair<fs::path, fs::path>> moved;   // (from, to), to undo
                std::vector<fs::path> placed;   // targets that weren't there before
                auto undo = [&] {
                    for (const fs::path& target : placed)
                        fs::remove(target, ec);
                    for (auto it = moved.rbegin(); it != moved.rend(); ++it)
                        fs::rename(it->second, it->first, ec);
                };
                for (const fs::path& path : aside_)
                {
                    const fs::path to = originals / path.lexically_relative(mod_);
                    fs::create_directories(to.parent_path(), ec);
                    if (!ec)
                        fs::rename(path, to, ec);
                    if (ec)
                    {
                        result_.problems.push_back(
                            fmt::format("Can't move {} aside: {}", Relative(path, mod_), ec.message()));
                        undo();
                        return;
                    }
                    moved.emplace_back(path, to);
                }
                for (Job& job : jobs_)
                {
                    if (job.temp.empty())
                        continue;
                    const bool replaces = std::ranges::find(aside_, job.target) != aside_.end();
                    fs::create_directories(job.target.parent_path(), ec);
                    if (!ec)
                        fs::rename(job.temp, job.target, ec);
                    if (ec)
                    {
                        result_.problems.push_back(
                            fmt::format("Can't put {} in place: {}", Relative(job.target, mod_), ec.message()));
                        undo();
                        return;
                    }
                    job.temp.clear();
                    if (!replaces)
                        placed.push_back(job.target);
                    ++result_.files;
                }
                result_.converted = true;
                LOGI("Mod {}: converted from the PC version: {} files written, {} left out (the originals are in {})",
                    Utf8(mod_.filename()), result_.files, result_.leftOut.size(), Utf8(originals));
            }

            const fs::path mod_, gameRoot_;
            const GameFiles& game_;
            const std::stop_token stop_;
            ConvertResult& result_;
            bool stopped_ = false;
            // Unpacked packages and converted files (removed at the end).
            const LockedFolder work_;
            int unpacked_ = 0;
            uint64_t readBytes_ = 0;
            bool tooMuch_ = false;   // more than kMaxReadBytes
            std::vector<PcArchive> archives_;
            std::vector<Job> jobs_;
            std::unordered_map<std::string, size_t> targets_;   // lower-case target -> job
            std::vector<fs::path> aside_;   // the mod's PC files, moved to originals
            std::vector<fs::path> materials_;   // the mod's loose .mao files
            std::vector<fs::path> atlases_;     // its loose menu atlas layouts
            // Add-in module files left out: more than text, and text (talk tables).
            bool moduleContent_ = false, moduleText_ = false;
            std::optional<fs::path> manifest_;   // an unpacked package's
        };
    }

    ConvertResult ConvertPcMod(const fs::path& modFolder, const fs::path& gameRoot, std::stop_token stop,
        const GameFiles* game)
    {
        ConvertResult result;
        std::optional<GameFiles> read;
        if (!game)
            game = &read.emplace(GameFiles::Read(gameRoot, stop));
        Conversion(modFolder, gameRoot, *game, stop, result).Run();
        return result;
    }

    void CleanUpConversions(const fs::path& modsRoot)
    {
        RemoveUnlockedFolders(modsRoot / kWorkFolder);
    }
}
