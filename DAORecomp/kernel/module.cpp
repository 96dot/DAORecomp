#include "stdafx.h"
#include "module.h"
#include "text.h"
#include "memory.h"
#include "thread.h"

#include <file.h>
#include <image.h>
#include <xex.h>
#include <xex_patcher.h>

namespace dao::kernel
{
    ModuleInfo g_module;

    static uint32_t s_timeStampBundle;

    // xboxkrnl variable exports (ordinal -> guest address), filled by CreateKernelVariables.
    static std::unordered_map<uint32_t, uint32_t> s_kernelVariables;

    static void W32(uint32_t address, uint32_t value) { *g_memory.Translate<u32>(address) = value; }

    static void CreateKernelVariables()
    {
        auto alloc = [](uint32_t size) { return KernelAlloc(size, 16); };

        // ExThreadObjectType: an OBJECT_TYPE; only the pool tag is meaningful.
        const uint32_t threadType = alloc(0x40);
        W32(threadType + 0x0C, 0x54687265); // 'Thre'
        s_kernelVariables[0x1B] = threadType;

        const uint32_t debugMonitor = alloc(4);
        s_kernelVariables[0x59] = debugMonitor;

        s_timeStampBundle = alloc(sizeof(X_TIME_STAMP_BUNDLE));
        s_kernelVariables[0xAD] = s_timeStampBundle;

        const uint32_t version = alloc(8);
        *g_memory.Translate<u16>(version + 0) = 2;
        *g_memory.Translate<u16>(version + 2) = 0;
        *g_memory.Translate<u16>(version + 4) = 17559;
        *g_memory.Translate<u16>(version + 6) = 0;
        s_kernelVariables[0x158] = version;

        const uint32_t commandLine = alloc(0x200);
        strcpy(g_memory.Translate<char>(commandLine), "\"default.xex\"");
        s_kernelVariables[0x1AE] = commandLine;

        const uint32_t moduleHandle = alloc(4);
        W32(moduleHandle, g_module.ldrEntry);
        s_kernelVariables[0x193] = moduleHandle;

        const uint32_t hardwareInfo = alloc(0x10);
        W32(hardwareInfo, 0x20);                            // flags
        *g_memory.Translate<uint8_t>(hardwareInfo + 4) = 6; // cpu count
        s_kernelVariables[0x156] = hardwareInfo;

        const uint32_t certMonitor = alloc(4);
        s_kernelVariables[0x266] = certMonitor;

        const uint32_t globalDevice = alloc(4);
        W32(globalDevice, alloc(40));
        s_kernelVariables[0x1BE] = globalDevice;

        const uint32_t globalXamDevice = alloc(4);
        s_kernelVariables[0x1BF] = globalXamDevice;

        const uint32_t gpuClock = alloc(4);
        W32(gpuClock, 500);
        s_kernelVariables[0x1C0] = gpuClock;

        // VdHSIOCalibrationLock: an initialised RTL_CRITICAL_SECTION.
        const uint32_t hsioLock = alloc(sizeof(X_RTL_CRITICAL_SECTION));
        auto* cs = g_memory.Translate<X_RTL_CRITICAL_SECTION>(hsioLock);
        cs->header.type = kTypeSynchronizationEvent;
        cs->header.size = 4;
        cs->header.waitList.flink = hsioLock + 8;
        cs->header.waitList.blink = hsioLock + 8;
        cs->lockCount = -1;
        s_kernelVariables[0x1C1] = hsioLock;
    }

    static uint32_t OptHeaderValue(const uint8_t* file, uint32_t key, uint32_t fallback = 0)
    {
        const auto* header = reinterpret_cast<const Xex2Header*>(file);
        const auto* opt = reinterpret_cast<const Xex2OptHeader*>(header + 1);
        for (uint32_t i = 0; i < header->headerCount; i++)
        {
            if (opt[i].key == key)
                return opt[i].value;
        }
        return fallback;
    }

    uint32_t XexHeaderField(uint32_t header, uint32_t key)
    {
        if (!header)
            return 0;
        const auto* h = g_memory.Translate<Xex2Header>(header);
        const auto* opt = reinterpret_cast<const Xex2OptHeader*>(h + 1);
        for (uint32_t i = 0; i < h->headerCount; i++)
        {
            if (opt[i].key != key)
                continue;
            switch (key & 0xFF)
            {
            case 0x00: return opt[i].value;
            case 0x01: return g_memory.ToGuest(&opt[i].value);
            default: return header + opt[i].value;
            }
        }
        return 0;
    }

    bool LoadXex(const std::filesystem::path& path, const std::filesystem::path& patchPath)
    {
        auto file = LoadFile(path);
        if (file.empty())
        {
            LOGE("Failed to read {}", Utf8(path));
            return false;
        }
        // Checked before patching or parsing: they trust what they read.
        if (!IsGameXex(file))
        {
            LOGE("{} isn't the one the code was recompiled from (the disc with media ID 186A1115)", Utf8(path));
            return false;
        }
        if (!patchPath.empty())
        {
            const auto patch = LoadFile(patchPath);
            // (Checked again on the bytes applied: SelectBuild hashed the file earlier.)
            if (!patch.empty() && !TitleUpdateNumber(patch))
            {
                LOGE("{} isn't Title Update 6's or 7's", Utf8(patchPath));
                return false;
            }
            std::vector<uint8_t> patched;
            constexpr bool kSkipData = false; // the data patches too, not only the code
            const auto result = patch.empty() ? XexPatcher::Result::FileOpenFailed
                : XexPatcher::apply(file.data(), file.size(), patch.data(), patch.size(), patched, kSkipData);
            if (result != XexPatcher::Result::Success)
            {
                LOGE("Failed to apply {} to {} (error {})", Utf8(patchPath), Utf8(path), int(result));
                return false;
            }
            file = std::move(patched);
        }

        Image image = Image::ParseImage(file.data(), file.size());
        if (!image.data)
        {
            LOGE("Failed to parse {}", Utf8(path));
            return false;
        }
        // Another executable (another title update) doesn't match the recompiled code.
        // Title Updates 6 and 7 share it, but not their image size: 7's is 64 KB more,
        // all of it relocations, which nothing reads (docs/porting-playbook.md, "Later
        // title updates"). So their build takes either size.
        const DaoBuildInfo& build = g_build.info;
        constexpr uint32_t kTu6ImageSize = 0x13F0000;
        constexpr uint32_t kTu7ImageSize = 0x1400000;
        const auto updateSize = [](uint32_t size) { return size == kTu6ImageSize || size == kTu7ImageSize; };
        const bool sizeMatches = image.size == build.imageSize ||
            (IsTitleUpdate() && updateSize(image.size) && updateSize(build.imageSize));
        if (image.base != build.imageBase || !sizeMatches)
        {
            LOGE("Unexpected image {:08X}+{:#x} (recompiled for {:08X}+{:#x})", image.base, image.size,
                build.imageBase, build.imageSize);
            return false;
        }

        // Basic compression yields sum(data+zero) bytes, possibly less than image.size.
        const auto* ffi = reinterpret_cast<const Xex2OptFileFormatInfo*>(getOptHeaderPtr(file.data(), XEX_HEADER_FILE_FORMAT_INFO));
        size_t valid = image.size;
        if (ffi && ffi->compressionType == XEX_COMPRESSION_BASIC)
        {
            auto* blocks = reinterpret_cast<const Xex2FileBasicCompressionBlock*>(ffi + 1);
            const size_t n = (ffi->infoSize / sizeof(Xex2FileBasicCompressionInfo)) - 1;
            valid = 0;
            for (size_t i = 0; i < n; i++)
                valid += blocks[i].dataSize + blocks[i].zeroSize;
        }
        // (Not beyond the recompiled image: its function table follows it, memory.h.)
        const uint32_t loadedSize = std::min(image.size, build.imageSize);
        valid = std::min<size_t>(valid, loadedSize);
        memcpy(g_memory.Translate(build.imageBase), image.data.get(), valid);
        LOGI("Loaded image {:08X}+{:#x} ({:#x} bytes of data), entry {:08X}", image.base, image.size, valid, image.entry_point);

        const auto* header = reinterpret_cast<const Xex2Header*>(file.data());
        g_module.entryPoint = uint32_t(image.entry_point);
        g_module.stackSize = OptHeaderValue(file.data(), XEX_HEADER_DEFAULT_STACK_SIZE, 0x40000);
        g_module.systemFlags = OptHeaderValue(file.data(), XEX_HEADER_SYSTEM_FLAGS);

        // Copy the XEX header for RtlImageXexHeaderField.
        g_module.xexHeaderSize = header->headerSize;
        g_module.xexHeader = KernelAlloc(g_module.xexHeaderSize, 0x1000);
        memcpy(g_memory.Translate(g_module.xexHeader), file.data(), g_module.xexHeaderSize);

        if (const uint32_t exec = XexHeaderField(g_module.xexHeader, XEX_HEADER_EXECUTION_INFO))
        {
            g_module.mediaId = *g_memory.Translate<u32>(exec + 0);
            g_module.version = *g_memory.Translate<u32>(exec + 4);
            g_module.titleId = *g_memory.Translate<u32>(exec + 12);
            LOGI("Title {:08X} media {:08X} version {:08X}", g_module.titleId, g_module.mediaId, g_module.version);
        }

        // Loader data table entry, the module "handle".
        g_module.ldrEntry = KernelAlloc(0x80);
        W32(g_module.ldrEntry + 0x18, build.imageBase);  // dll_base
        W32(g_module.ldrEntry + 0x1C, build.imageBase);  // image_base
        W32(g_module.ldrEntry + 0x20, loadedSize);      // image_size
        W32(g_module.ldrEntry + 0x34, 0x40000000);      // flags
        W32(g_module.ldrEntry + 0x38, loadedSize);      // full_image_size
        W32(g_module.ldrEntry + 0x3C, g_module.entryPoint);
        W32(g_module.ldrEntry + 0x58, g_module.xexHeader);
        for (uint32_t links = 0; links < 0x18; links += 8)
        {
            W32(g_module.ldrEntry + links, g_module.ldrEntry + links);
            W32(g_module.ldrEntry + links + 4, g_module.ldrEntry + links);
        }

        // TLS
        if (const uint32_t tls = XexHeaderField(g_module.xexHeader, XEX_HEADER_TLS_INFO))
        {
            auto* t = g_memory.Translate<u32>(tls);
            ConfigureTls(t[0], t[1], t[3], t[2]);
        }
        else
        {
            ConfigureTls(64, 0, 0, 0);
        }

        // Kernel variable imports.
        CreateKernelVariables();
        auto* imports = reinterpret_cast<const Xex2ImportHeader*>(getOptHeaderPtr(file.data(), XEX_HEADER_IMPORT_LIBRARIES));
        if (imports)
        {
            std::vector<std::string> names;
            auto* str = reinterpret_cast<const char*>(imports + 1);
            size_t off = 0;
            for (size_t i = 0; i < imports->numImports; i++)
            {
                names.emplace_back(str + off);
                off += ((names.back().size() + 1) + 3) & ~size_t(3);
            }

            auto* lib = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const char*>(imports) +
                sizeof(Xex2ImportHeader) + imports->sizeOfStringTable);
            for (size_t i = 0; i < names.size(); i++)
            {
                auto* desc = reinterpret_cast<const Xex2ImportDescriptor*>(lib + 1);
                const bool kernel = names[i].starts_with("xboxkrnl");
                for (size_t j = 0; j < lib->numberOfImports; j++)
                {
                    const uint32_t slot = desc[j].firstThunk;
                    auto* p = g_memory.Translate<uint8_t>(slot);
                    if (p[0] == 0x60 && p[1] == 0 && p[2] == 0 && p[3] == 0)
                        continue; // function thunk (stubbed by the image loader)

                    uint32_t raw; // Xex2LoadImage left variable records in host order
                    memcpy(&raw, p, 4);
                    const uint32_t ordinal = raw & 0xFFFF;
                    auto it = s_kernelVariables.find(ordinal);
                    if (kernel && (raw >> 24) == 0 && it != s_kernelVariables.end())
                    {
                        W32(slot, it->second);
                        LOGI("Kernel variable {:#x} at {:08X} -> {:08X}", ordinal, slot, it->second);
                    }
                    else
                    {
                        // Function import address slot: unused (calls go straight to __imp__).
                        W32(slot, 0);
                    }
                }
                lib = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const char*>(lib + 1) +
                    lib->numberOfImports * sizeof(Xex2ImportDescriptor));
            }
        }

        return true;
    }

    void StartTimeStampThread()
    {
        std::thread([] {
            SetThreadDescription(GetCurrentThread(), L"DAO KeTimeStampBundle");
            const auto start = std::chrono::steady_clock::now();
            auto* bundle = g_memory.Translate<X_TIME_STAMP_BUNDLE>(s_timeStampBundle);
            for (;;)
            {
                const auto elapsed = std::chrono::steady_clock::now() - start;
                const uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                bundle->interruptTime = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100);
                bundle->systemTime = SystemTime100ns();
                bundle->tickCount = uint32_t(ms);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    }
}
