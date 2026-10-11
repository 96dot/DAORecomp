// xboxkrnl: file I/O on top of the VFS. All I/O completes synchronously; for
// overlapped requests the event is set and the completion APC queued, as Xenia does.
#include "stdafx.h"
#include <set>
#include "gpu/xenia_host.h"
#include "text.h"
#include "function.h"
#include "thread.h"
#include "vfs.h"
#include "patches/patches.h"

namespace dao::imp
{
    using namespace dao::kernel;
    namespace fs = std::filesystem;

    // NtCreateFile create options
    constexpr uint32_t FILE_DIRECTORY_FILE = 0x1;
    constexpr uint32_t FILE_SYNCHRONOUS_IO_ALERT = 0x10;
    constexpr uint32_t FILE_SYNCHRONOUS_IO_NONALERT = 0x20;
    constexpr uint32_t FILE_NON_DIRECTORY_FILE = 0x40;
    constexpr uint32_t FILE_DELETE_ON_CLOSE = 0x1000;

    enum Disposition : uint32_t { Supersede, Open, Create, OpenIf, Overwrite, OverwriteIf };
    enum Action : uint32_t { Superseded, Opened, Created, Overwritten, Exists, DoesNotExist };

    constexpr uint32_t kWriteAccess = 0x40000000 | 0x10000000 | 0x2 | 0x4 | 0x100; // GENERIC_WRITE|GENERIC_ALL|WRITE_DATA|APPEND|WRITE_ATTRIBUTES
    constexpr uint64_t kUseFilePointer = 0xFFFFFFFFFFFFFFFEull;

    constexpr uint32_t X_FILE_ATTRIBUTE_READONLY = 0x01;
    constexpr uint32_t X_FILE_ATTRIBUTE_DIRECTORY = 0x10;
    constexpr uint32_t X_FILE_ATTRIBUTE_NORMAL = 0x80;

    static std::string AnsiString(const X_ANSI_STRING* str)
    {
        if (!str || !str->buffer || !str->length)
            return {};
        return std::string(g_memory.Translate<char>(str->buffer), uint16_t(str->length));
    }

    static void SetIosb(X_IO_STATUS_BLOCK* iosb, uint32_t status, uint32_t information)
    {
        if (iosb)
        {
            iosb->status = status;
            iosb->information = information;
        }
    }

    // Full guest path for an OBJECT_ATTRIBUTES (handles RootDirectory-relative names).
    static std::optional<std::string> GuestPathOf(const X_OBJECT_ATTRIBUTES* attributes)
    {
        if (!attributes)
            return std::nullopt;
        std::string name = AnsiString(g_memory.Translate<X_ANSI_STRING>(attributes->objectName));
        const uint32_t root = attributes->rootDirectory;
        if (root && root != 0xFFFFFFFD)
        {
            auto dir = LookupHandle<File>(root, ObjectType::File);
            if (!dir)
                return std::nullopt;
            name = dir->guestPath + "\\" + name;
        }
        return name;
    }

    static uint32_t HostAttributes(DWORD attributes)
    {
        uint32_t out = attributes & (X_FILE_ATTRIBUTE_READONLY | 0x2 | 0x4 | X_FILE_ATTRIBUTE_DIRECTORY | 0x20);
        return out ? out : X_FILE_ATTRIBUTE_NORMAL;
    }

    static uint64_t FileTimeValue(const FILETIME& ft)
    {
        return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    struct HostInfo
    {
        uint64_t creation, access, write, size;
        uint32_t attributes;
    };

    static bool QueryHost(const fs::path& path, HostInfo& info)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            return false;
        info.creation = FileTimeValue(data.ftCreationTime);
        info.access = FileTimeValue(data.ftLastAccessTime);
        info.write = FileTimeValue(data.ftLastWriteTime);
        info.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        info.attributes = HostAttributes(data.dwFileAttributes);
        if (VfsIsReadOnly(path))
            info.attributes |= X_FILE_ATTRIBUTE_READONLY;
        if (info.attributes & X_FILE_ATTRIBUTE_DIRECTORY)
            info.attributes &= ~X_FILE_ATTRIBUTE_NORMAL;
        return true;
    }

    uint32_t NtCreateFile(u32* handleOut, uint32_t access, X_OBJECT_ATTRIBUTES* attributes, X_IO_STATUS_BLOCK* iosb,
        u64* allocationSize, uint32_t fileAttributes, uint32_t shareAccess, uint32_t disposition, uint32_t options)
    {
        auto guestPath = GuestPathOf(attributes);
        if (!guestPath)
        {
            SetIosb(iosb, X_STATUS_INVALID_PARAMETER, 0);
            return X_STATUS_INVALID_PARAMETER;
        }
        if (dao::patches::IsHiddenFile(*guestPath))
        {
            SetIosb(iosb, X_STATUS_OBJECT_NAME_NOT_FOUND, Action::DoesNotExist);
            return X_STATUS_OBJECT_NAME_NOT_FOUND;
        }

        auto hostPath = VfsResolve(*guestPath);
        if (!hostPath)
        {
            LOGW("NtCreateFile: unmapped path '{}'", *guestPath);
            SetIosb(iosb, X_STATUS_OBJECT_PATH_NOT_FOUND, Action::DoesNotExist);
            return X_STATUS_OBJECT_PATH_NOT_FOUND;
        }

        std::error_code ec;
        const bool exists = fs::exists(*hostPath, ec);
        const bool isDir = exists && fs::is_directory(*hostPath, ec);
        const bool wantsWrite = (access & kWriteAccess) != 0 || disposition != Disposition::Open;
        const bool readOnlyDevice = VfsIsReadOnly(*hostPath);

        auto fail = [&](uint32_t status, uint32_t action) {
            LOGT("NtCreateFile '{}' -> {:08X}", *guestPath, status);
            SetIosb(iosb, status, action);
            if (handleOut)
                *handleOut = 0;
            return status;
        };

        if (!exists && (disposition == Disposition::Open || disposition == Disposition::Overwrite))
        {
            if (!fs::exists(hostPath->parent_path(), ec))
                return fail(X_STATUS_OBJECT_PATH_NOT_FOUND, Action::DoesNotExist);
            return fail(X_STATUS_OBJECT_NAME_NOT_FOUND, Action::DoesNotExist);
        }
        if (exists && disposition == Disposition::Create)
            return fail(X_STATUS_OBJECT_NAME_COLLISION, Action::Exists);
        if (isDir && (options & FILE_NON_DIRECTORY_FILE))
            return fail(X_STATUS_FILE_IS_A_DIRECTORY, Action::Exists);
        if (exists && !isDir && (options & FILE_DIRECTORY_FILE))
            return fail(X_STATUS_NOT_A_DIRECTORY, Action::Exists);
        if (readOnlyDevice && (!exists || (wantsWrite && disposition != Disposition::Open && disposition != Disposition::OpenIf)))
            return fail(X_STATUS_ACCESS_DENIED, Action::DoesNotExist);

        auto file = std::make_shared<File>();
        file->guestPath = *guestPath;
        file->hostPath = *hostPath;
        file->synchronous = (options & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)) != 0;
        file->deleteOnClose = (options & FILE_DELETE_ON_CLOSE) != 0 && !readOnlyDevice;

        uint32_t action = Action::Opened;
        if (isDir || (!exists && (options & FILE_DIRECTORY_FILE)))
        {
            if (!exists)
            {
                fs::create_directories(*hostPath, ec);
                if (ec)
                    return fail(X_STATUS_ACCESS_DENIED, Action::DoesNotExist);
                action = Action::Created;
            }
            file->isDirectory = true;
        }
        else
        {
            DWORD creation = OPEN_EXISTING;
            switch (disposition)
            {
            case Disposition::Supersede: creation = CREATE_ALWAYS; action = exists ? Action::Superseded : Action::Created; break;
            case Disposition::Open: creation = OPEN_EXISTING; action = Action::Opened; break;
            case Disposition::Create: creation = CREATE_NEW; action = Action::Created; break;
            case Disposition::OpenIf: creation = OPEN_ALWAYS; action = exists ? Action::Opened : Action::Created; break;
            case Disposition::Overwrite: creation = TRUNCATE_EXISTING; action = Action::Overwritten; break;
            case Disposition::OverwriteIf: creation = CREATE_ALWAYS; action = exists ? Action::Overwritten : Action::Created; break;
            default: return fail(X_STATUS_INVALID_PARAMETER, 0);
            }

            const bool write = wantsWrite && !readOnlyDevice;
            if (write && !exists)
                fs::create_directories(hostPath->parent_path(), ec);

            file->host = CreateFileW(hostPath->c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0),
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file->host == INVALID_HANDLE_VALUE)
            {
                const DWORD error = GetLastError();
                LOGW("NtCreateFile: host open of '{}' failed ({})", dao::Utf8(*hostPath), error);
                return fail(error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ? X_STATUS_ACCESS_DENIED
                    : X_STATUS_OBJECT_NAME_NOT_FOUND, Action::DoesNotExist);
            }
        }

        LOGT("NtCreateFile '{}' access {:08X} disp {} opts {:#x} -> {}", *guestPath, access, disposition, options,
            dao::Utf8(*hostPath));
        SetIosb(iosb, X_STATUS_SUCCESS, action);
        if (handleOut)
            *handleOut = CreateHandle(file);
        return X_STATUS_SUCCESS;
    }

    uint32_t NtOpenFile(u32* handleOut, uint32_t access, X_OBJECT_ATTRIBUTES* attributes, X_IO_STATUS_BLOCK* iosb,
        uint32_t options)
    {
        return NtCreateFile(handleOut, access, attributes, iosb, nullptr, 0, 0, Disposition::Open, options);
    }

    // Completes an I/O request: status block, event, APC.
    static uint32_t CompleteIo(File& file, uint32_t status, uint32_t information, uint32_t eventHandle,
        uint32_t apcRoutine, uint32_t apcContext, X_IO_STATUS_BLOCK* iosb)
    {
        SetIosb(iosb, status, information);
        if (eventHandle)
        {
            if (auto e = LookupHandle<Event>(eventHandle, ObjectType::Event))
                e->Set();
        }
        if ((apcRoutine & ~1u) && apcContext)
        {
            if (auto* thread = GuestThread::Current())
                thread->QueueApc({ 0, apcRoutine & ~1u, apcContext, g_memory.ToGuest(iosb), 0, 0 });
        }
        if (status == X_STATUS_SUCCESS && !file.synchronous)
            return X_STATUS_PENDING;
        return status;
    }

    static uint32_t ReadAt(File& file, uint8_t* buffer, uint32_t length, const u64* offsetPtr, uint32_t& bytesRead)
    {
        std::lock_guard lock(file.mutex);
        uint64_t offset = offsetPtr ? uint64_t(*offsetPtr) : kUseFilePointer;
        if (offset == kUseFilePointer || offset == ~0ull)
            offset = file.position;

        OVERLAPPED ov{};
        ov.Offset = DWORD(offset);
        ov.OffsetHigh = DWORD(offset >> 32);
        DWORD read = 0;
        // ReadFile can't write into pages the GPU write-protected; drop the watches first.
        dao::gpu::PrepareGuestWrite(g_memory.ToGuest(buffer), length);
        if (!ReadFile(file.host, buffer, length, &read, &ov))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_HANDLE_EOF)
            {
                LOGW("ReadFile '{}' failed ({})", file.guestPath, error);
                bytesRead = 0;
                return X_STATUS_UNSUCCESSFUL;
            }
        }
        bytesRead = read;
        file.position = offset + read;
        if (read == 0 && length != 0)
            return X_STATUS_END_OF_FILE;
        return X_STATUS_SUCCESS;
    }

    uint32_t NtReadFile(uint32_t handle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext,
        X_IO_STATUS_BLOCK* iosb, uint8_t* buffer, uint32_t length, u64* offset)
    {
        auto file = LookupHandle<File>(handle, ObjectType::File);
        if (!file || file->isDirectory)
            return X_STATUS_INVALID_HANDLE;
        uint32_t read = 0;
        const uint32_t status = ReadAt(*file, buffer, length, offset, read);
        LOGT("NtReadFile '{}' {:#x} bytes @ {:#x} -> {:#x} ({:08X})", file->guestPath, length,
            offset ? uint64_t(*offset) : file->position, read, status);
        if (status != X_STATUS_SUCCESS && status != X_STATUS_END_OF_FILE)
        {
            SetIosb(iosb, status, 0);
            return status;
        }
        return CompleteIo(*file, status, read, eventHandle, apcRoutine, apcContext, iosb);
    }

    uint32_t NtReadFileScatter(uint32_t handle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext,
        X_IO_STATUS_BLOCK* iosb, u32* segments, uint32_t length, u64* offset)
    {
        auto file = LookupHandle<File>(handle, ObjectType::File);
        if (!file || file->isDirectory)
            return X_STATUS_INVALID_HANDLE;

        uint64_t position = offset ? uint64_t(*offset) : kUseFilePointer;
        if (position == kUseFilePointer || position == ~0ull)
            position = file->position;

        uint32_t total = 0;
        uint32_t status = X_STATUS_SUCCESS;
        for (uint32_t done = 0, i = 0; done < length; done += 0x1000, i++)
        {
            const uint32_t chunk = std::min<uint32_t>(0x1000, length - done);
            u64 at = position + done;
            uint32_t read = 0;
            status = ReadAt(*file, g_memory.Translate<uint8_t>(segments[i]), chunk, &at, read);
            total += read;
            if (status != X_STATUS_SUCCESS || read < chunk)
                break;
        }
        if (total && status == X_STATUS_END_OF_FILE)
            status = X_STATUS_SUCCESS;
        return CompleteIo(*file, status, total, eventHandle, apcRoutine, apcContext, iosb);
    }

    uint32_t NtWriteFile(uint32_t handle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext,
        X_IO_STATUS_BLOCK* iosb, const uint8_t* buffer, uint32_t length, u64* offsetPtr)
    {
        auto file = LookupHandle<File>(handle, ObjectType::File);
        if (!file || file->isDirectory)
            return X_STATUS_INVALID_HANDLE;

        DWORD written = 0;
        {
            std::lock_guard lock(file->mutex);
            uint64_t offset = offsetPtr ? uint64_t(*offsetPtr) : kUseFilePointer;
            if (offset == kUseFilePointer || offset == ~0ull)
                offset = file->position;
            OVERLAPPED ov{};
            ov.Offset = DWORD(offset);
            ov.OffsetHigh = DWORD(offset >> 32);
            if (!WriteFile(file->host, buffer, length, &written, &ov))
            {
                LOGW("WriteFile '{}' failed ({})", file->guestPath, GetLastError());
                SetIosb(iosb, X_STATUS_ACCESS_DENIED, 0);
                return X_STATUS_ACCESS_DENIED;
            }
            file->position = offset + written;
        }
        return CompleteIo(*file, X_STATUS_SUCCESS, written, eventHandle, apcRoutine, apcContext, iosb);
    }

    uint32_t NtFlushBuffersFile(uint32_t handle, X_IO_STATUS_BLOCK* iosb)
    {
        auto file = LookupHandle<File>(handle, ObjectType::File);
        if (!file)
            return X_STATUS_INVALID_HANDLE;
        if (file->host != INVALID_HANDLE_VALUE)
            FlushFileBuffers(file->host);
        SetIosb(iosb, X_STATUS_SUCCESS, 0);
        return X_STATUS_SUCCESS;
    }

    struct X_FILE_NETWORK_OPEN_INFORMATION
    {
        u64 creationTime, lastAccessTime, lastWriteTime, changeTime;
        u64 allocationSize, endOfFile;
        u32 attributes, pad;
    };
    static_assert(sizeof(X_FILE_NETWORK_OPEN_INFORMATION) == 56);

    static void FillNetworkOpen(X_FILE_NETWORK_OPEN_INFORMATION* info, const HostInfo& host)
    {
        info->creationTime = host.creation;
        info->lastAccessTime = host.access;
        info->lastWriteTime = host.write;
        info->changeTime = host.write;
        info->allocationSize = (host.size + 0xFFF) & ~0xFFFull;
        info->endOfFile = host.size;
        info->attributes = host.attributes;
        info->pad = 0;
    }

    uint32_t NtQueryFullAttributesFile(X_OBJECT_ATTRIBUTES* attributes, X_FILE_NETWORK_OPEN_INFORMATION* info)
    {
        auto guestPath = GuestPathOf(attributes);
        if (!guestPath)
            return X_STATUS_INVALID_PARAMETER;
        auto hostPath = VfsResolve(*guestPath);
        HostInfo host;
        if (!hostPath || dao::patches::IsHiddenFile(*guestPath) || !QueryHost(*hostPath, host))
        {
            LOGT("NtQueryFullAttributesFile '{}' -> not found", *guestPath);
            return X_STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (info)
            FillNetworkOpen(info, host);
        return X_STATUS_SUCCESS;
    }

    // File information classes
    enum : uint32_t
    {
        XFileBasicInformation = 4,
        XFileStandardInformation = 5,
        XFileInternalInformation = 6,
        XFileRenameInformation = 10,
        XFileDispositionInformation = 13,
        XFilePositionInformation = 14,
        XFileModeInformation = 16,
        XFileAlignmentInformation = 17,
        XFileAllocationInformation = 19,
        XFileEndOfFileInformation = 20,
        XFileSectorInformation = 26,
        XFileIoPriorityInformation = 32,
        XFileNetworkOpenInformation = 34,
    };

    uint32_t NtQueryInformationFile(uint32_t handle, X_IO_STATUS_BLOCK* iosb, uint8_t* info, uint32_t length,
        uint32_t infoClass)
    {
        auto file = LookupHandle<File>(handle, ObjectType::File);
        if (!file)
            return X_STATUS_INVALID_HANDLE;

        HostInfo host{};
        QueryHost(file->hostPath, host);
        memset(info, 0, length);

        uint32_t written = 0;
        uint32_t status = X_STATUS_SUCCESS;
        auto need = [&](uint32_t size) { written = size; return length >= size; };

        switch (infoClass)
        {
        case XFileBasicInformation:
            if (!need(40)) { status = X_STATUS_BUFFER_TOO_SMALL; break; }
            {
                auto* b = reinterpret_cast<u64*>(info);
                b[0] = host.creation;
                b[1] = host.access;
                b[2] = host.write;
                b[3] = host.write;
                b[4] = host.attributes;
            }
            break;
        case XFileStandardInformation:
            if (!need(24)) { status = X_STATUS_BUFFER_TOO_SMALL; break; }
            {
                auto* s = reinterpret_cast<u64*>(info);
                s[0] = (host.size + 0xFFF) & ~0xFFFull;
                s[1] = host.size;
                *reinterpret_cast<u32*>(info + 16) = 1;
                info[20] = file->deleteOnClose;
                info[21] = file->isDirectory;
            }
            break;
        case XFileInternalInformation:
            if (!need(8)) { status = X_STATUS_BUFFER_TOO_SMALL; break; }
            *reinterpret_cast<u64*>(info) = std::hash<std::wstring>{}(file->hostPath.native());
            break;
        case XFilePositionInformation:
            if (!need(8)) { status = X_STATUS_BUFFER_TOO_SMALL; break; }
            *reinterpret_cast<u64*>(info) = file->position;
            break;
        case XFileModeInformation:
        case XFileAlignmentInformation:
        case XFileIoPriorityInformation:
            if (!need(4)) { status = X_STATUS_BUFFER_TOO_SMALL; break; }
            break;
        case XFileSectorInformation:
            if (!need(4)) { status = X_STATUS_BUFFER_TOO_SMALL; break; }
            *reinterpret_cast<u32*>(info) = uint32_t(std::hash<std::wstring>{}(file->hostPath.native()));
            break;
        case XFileNetworkOpenInformation:
            if (!need(56)) { status = X_STATUS_BUFFER_TOO_SMALL; break; }
            FillNetworkOpen(reinterpret_cast<X_FILE_NETWORK_OPEN_INFORMATION*>(info), host);
            break;
        default:
            LOGW("NtQueryInformationFile: unsupported class {}", infoClass);
            status = X_STATUS_INVALID_PARAMETER;
            written = 0;
            break;
        }
        SetIosb(iosb, status, status == X_STATUS_SUCCESS ? written : 0);
        return status;
    }

    uint32_t NtSetInformationFile(uint32_t handle, X_IO_STATUS_BLOCK* iosb, uint8_t* info, uint32_t length,
        uint32_t infoClass)
    {
        auto file = LookupHandle<File>(handle, ObjectType::File);
        if (!file)
            return X_STATUS_INVALID_HANDLE;

        uint32_t status = X_STATUS_SUCCESS;
        uint32_t written = length;
        switch (infoClass)
        {
        case XFilePositionInformation:
        {
            std::lock_guard lock(file->mutex);
            file->position = *reinterpret_cast<u64*>(info);
            written = 8;
            break;
        }
        case XFileEndOfFileInformation:
        {
            const uint64_t size = *reinterpret_cast<u64*>(info);
            FILE_END_OF_FILE_INFO eof{};
            eof.EndOfFile.QuadPart = int64_t(size);
            if (file->host == INVALID_HANDLE_VALUE ||
                !SetFileInformationByHandle(file->host, FileEndOfFileInfo, &eof, sizeof(eof)))
                status = X_STATUS_ACCESS_DENIED;
            written = 8;
            break;
        }
        case XFileDispositionInformation:
            file->deleteOnClose = info[0] != 0 && !VfsIsReadOnly(file->hostPath);
            written = 1;
            break;
        case XFileAllocationInformation:
        case XFileBasicInformation:
        case XFileModeInformation:
        case XFileIoPriorityInformation:
            break;
        case XFileRenameInformation:
        {
            const auto* ansi = reinterpret_cast<X_ANSI_STRING*>(info + 8);
            auto target = VfsResolve(AnsiString(ansi));
            std::error_code ec;
            if (!target)
            {
                status = X_STATUS_OBJECT_PATH_NOT_FOUND;
                break;
            }
            if (file->host != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(file->host);
                file->host = INVALID_HANDLE_VALUE;
            }
            fs::rename(file->hostPath, *target, ec);
            if (ec)
                status = X_STATUS_ACCESS_DENIED;
            else
                file->hostPath = *target;
            if (!file->isDirectory)
                file->host = CreateFileW(file->hostPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            break;
        }
        default:
            LOGW("NtSetInformationFile: unsupported class {}", infoClass);
            status = X_STATUS_INVALID_PARAMETER;
            written = 0;
            break;
        }
        SetIosb(iosb, status, written);
        return status;
    }

    uint32_t NtQueryVolumeInformationFile(uint32_t handle, X_IO_STATUS_BLOCK* iosb, uint8_t* info, uint32_t length,
        uint32_t infoClass)
    {
        auto file = LookupHandle<File>(handle, ObjectType::File);
        if (!file)
            return X_STATUS_INVALID_HANDLE;
        memset(info, 0, length);
        const bool disc = VfsIsReadOnly(file->hostPath);
        uint32_t written = 0;
        switch (infoClass)
        {
        case 1: // volume
            written = std::min<uint32_t>(length, 24);
            break;
        case 3: // size
        {
            if (length < 24)
                return X_STATUS_BUFFER_TOO_SMALL;
            auto* s = reinterpret_cast<u64*>(info);
            s[0] = disc ? 0x00800000 : 0x01000000; // total allocation units
            s[1] = disc ? 0 : 0x00800000;          // available
            *reinterpret_cast<u32*>(info + 16) = 32;    // sectors per unit (16 KB clusters)
            *reinterpret_cast<u32*>(info + 20) = 0x200; // bytes per sector
            written = 24;
            break;
        }
        case 4: // device
            if (length < 8)
                return X_STATUS_BUFFER_TOO_SMALL;
            *reinterpret_cast<u32*>(info) = disc ? 0x2 : 0x7;      // CD_ROM / DISK
            *reinterpret_cast<u32*>(info + 4) = disc ? 0x3 : 0x0;  // removable | read-only
            written = 8;
            break;
        case 5: // attribute
        {
            const char* name = disc ? "GDFX" : "FATX";
            if (length < 16)
                return X_STATUS_BUFFER_TOO_SMALL;
            *reinterpret_cast<u32*>(info) = disc ? 0x00080000 : 0x2; // read-only volume / case-preserved
            *reinterpret_cast<s32*>(info + 4) = 255;
            *reinterpret_cast<u32*>(info + 8) = 4;
            memcpy(info + 12, name, std::min<uint32_t>(4, length - 12));
            written = 16;
            break;
        }
        default:
            LOGW("NtQueryVolumeInformationFile: unsupported class {}", infoClass);
            SetIosb(iosb, X_STATUS_INVALID_PARAMETER, 0);
            return X_STATUS_INVALID_PARAMETER;
        }
        SetIosb(iosb, X_STATUS_SUCCESS, written);
        return X_STATUS_SUCCESS;
    }

    static bool WildcardMatch(std::string_view pattern, std::string_view name)
    {
        if (pattern.empty() || pattern == "*" || pattern == "*.*")
            return true;
        size_t p = 0, n = 0, star = std::string_view::npos, mark = 0;
        while (n < name.size())
        {
            if (p < pattern.size() && (pattern[p] == '?' || tolower(uint8_t(pattern[p])) == tolower(uint8_t(name[n]))))
            {
                p++;
                n++;
            }
            else if (p < pattern.size() && pattern[p] == '*')
            {
                star = p++;
                mark = n;
            }
            else if (star != std::string_view::npos)
            {
                p = star + 1;
                n = ++mark;
            }
            else
            {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*')
            p++;
        return p == pattern.size();
    }

    uint32_t NtQueryDirectoryFile(uint32_t handle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext,
        X_IO_STATUS_BLOCK* iosb, uint8_t* info, uint32_t length, X_ANSI_STRING* fileName, bool restart)
    {
        auto file = LookupHandle<File>(handle, ObjectType::File);
        if (!file || !file->isDirectory)
        {
            SetIosb(iosb, X_STATUS_NO_SUCH_FILE, 0);
            return X_STATUS_NO_SUCH_FILE;
        }
        if (length < 72)
            return X_STATUS_BUFFER_TOO_SMALL;

        const std::string pattern = AnsiString(fileName);
        std::lock_guard lock(file->mutex);
        if (restart || !file->enumerated)
        {
            // Mods layer over the disc: list every layer, the first one wins per name.
            file->entries.clear();
            std::set<std::string> seen;
            for (const auto& dir : VfsResolveAll(file->guestPath))
            {
                std::error_code ec;
                for (auto& entry : fs::directory_iterator(dir, ec))
                {
                    // (A name the guest can't use is left out.)
                    std::optional<std::string> guestName = dao::ToAnsi(entry.path().filename());
                    if (!guestName)
                        continue;
                    std::string name = std::move(*guestName);
                    if (dao::patches::IsHiddenFile(name))
                        continue;
                    std::transform(name.begin(), name.end(), name.begin(), [](char c) { return char(tolower(uint8_t(c))); });
                    if (seen.insert(std::move(name)).second)
                        file->entries.push_back(entry);
                }
            }
            file->nextEntry = 0;
            file->enumerated = true;
        }

        while (file->nextEntry < file->entries.size())
        {
            const auto& entry = file->entries[file->nextEntry++];
            const std::string name = dao::ToAnsi(entry.path().filename()).value_or(std::string());
            if (!WildcardMatch(pattern, name))
                continue;
            if (0x40 + name.size() > length)
            {
                file->nextEntry--;
                SetIosb(iosb, X_STATUS_BUFFER_OVERFLOW, 0);
                return X_STATUS_BUFFER_OVERFLOW;
            }
            HostInfo host{};
            QueryHost(entry.path(), host);
            memset(info, 0, 0x40);
            auto* q = reinterpret_cast<u64*>(info + 8);
            *reinterpret_cast<u32*>(info + 0) = 0; // next entry offset: one entry per call
            *reinterpret_cast<u32*>(info + 4) = uint32_t(file->nextEntry - 1);
            q[0] = host.creation;
            q[1] = host.access;
            q[2] = host.write;
            q[3] = host.write;
            q[4] = host.size;
            q[5] = (host.size + 0xFFF) & ~0xFFFull;
            *reinterpret_cast<u32*>(info + 0x38) = host.attributes;
            *reinterpret_cast<u32*>(info + 0x3C) = uint32_t(name.size());
            memcpy(info + 0x40, name.data(), name.size());
            const uint32_t written = uint32_t(0x40 + name.size());
            return CompleteIo(*file, X_STATUS_SUCCESS, written, eventHandle, apcRoutine, apcContext, iosb) == X_STATUS_PENDING
                ? X_STATUS_PENDING : X_STATUS_SUCCESS;
        }

        SetIosb(iosb, X_STATUS_NO_MORE_FILES, 0);
        if (eventHandle)
        {
            if (auto e = LookupHandle<Event>(eventHandle, ObjectType::Event))
                e->Set();
        }
        return X_STATUS_NO_MORE_FILES;
    }

    uint32_t ObCreateSymbolicLink(X_ANSI_STRING* link, X_ANSI_STRING* target)
    {
        VfsCreateSymlink(AnsiString(link), AnsiString(target));
        return X_STATUS_SUCCESS;
    }

    uint32_t ObDeleteSymbolicLink(X_ANSI_STRING* link)
    {
        return VfsDeleteSymlink(AnsiString(link)) ? X_STATUS_SUCCESS : X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
}

DAO_IMPORT_AS(NtCreateFile, dao::imp::NtCreateFile)
DAO_IMPORT_AS(NtOpenFile, dao::imp::NtOpenFile)
DAO_IMPORT_AS(NtReadFile, dao::imp::NtReadFile)
DAO_IMPORT_AS(NtReadFileScatter, dao::imp::NtReadFileScatter)
DAO_IMPORT_AS(NtWriteFile, dao::imp::NtWriteFile)
DAO_IMPORT_AS(NtFlushBuffersFile, dao::imp::NtFlushBuffersFile)
DAO_IMPORT_AS(NtQueryFullAttributesFile, dao::imp::NtQueryFullAttributesFile)
DAO_IMPORT_AS(NtQueryInformationFile, dao::imp::NtQueryInformationFile)
DAO_IMPORT_AS(NtSetInformationFile, dao::imp::NtSetInformationFile)
DAO_IMPORT_AS(NtQueryVolumeInformationFile, dao::imp::NtQueryVolumeInformationFile)
DAO_IMPORT_AS(NtQueryDirectoryFile, dao::imp::NtQueryDirectoryFile)
DAO_IMPORT_AS(ObCreateSymbolicLink, dao::imp::ObCreateSymbolicLink)
DAO_IMPORT_AS(ObDeleteSymbolicLink, dao::imp::ObDeleteSymbolicLink)
