// xboxkrnl: Rtl strings/time/memory helpers, printf family, debug, Xex module queries, XConfig.
#include "stdafx.h"
#include "function.h"
#include "module.h"
#include "thread.h"

namespace dao::imp
{
    using namespace dao::kernel;

    // ------------------------------------------------------------- strings

    static size_t GuestStrLen(uint32_t address)
    {
        return address ? strlen(g_memory.Translate<char>(address)) : 0;
    }

    static size_t GuestWStrLen(uint32_t address)
    {
        if (!address)
            return 0;
        auto* p = g_memory.Translate<u16>(address);
        size_t n = 0;
        while (p[n] != 0)
            n++;
        return n;
    }

    void RtlInitAnsiString(X_ANSI_STRING* dst, uint32_t src)
    {
        const size_t length = GuestStrLen(src);
        dst->length = uint16_t(length);
        dst->maximumLength = uint16_t(src ? length + 1 : 0);
        dst->buffer = src;
    }

    void RtlInitUnicodeString(X_UNICODE_STRING* dst, uint32_t src)
    {
        const size_t length = GuestWStrLen(src);
        dst->length = uint16_t(length * 2);
        dst->maximumLength = uint16_t(src ? (length + 1) * 2 : 0);
        dst->buffer = src;
    }

    void RtlFreeAnsiString(X_ANSI_STRING* str)
    {
        if (str->buffer)
            KernelFree(str->buffer);
        str->buffer = 0;
        str->length = 0;
        str->maximumLength = 0;
    }

    uint32_t RtlUnicodeStringToAnsiString(X_ANSI_STRING* dst, X_UNICODE_STRING* src, bool allocate)
    {
        const uint32_t chars = uint16_t(src->length) / 2;
        if (allocate)
        {
            dst->buffer = KernelAlloc(chars + 1);
            dst->maximumLength = uint16_t(chars + 1);
        }
        else if (uint16_t(dst->maximumLength) < chars + 1)
        {
            return X_STATUS_BUFFER_OVERFLOW;
        }
        auto* in = g_memory.Translate<u16>(src->buffer);
        auto* out = g_memory.Translate<char>(dst->buffer);
        for (uint32_t i = 0; i < chars; i++)
        {
            const uint16_t c = in[i];
            out[i] = c < 0x100 ? char(c) : '?';
        }
        out[chars] = 0;
        dst->length = uint16_t(chars);
        return X_STATUS_SUCCESS;
    }

    uint32_t RtlMultiByteToUnicodeN(u16* dst, uint32_t dstBytes, u32* written, const uint8_t* src, uint32_t srcBytes)
    {
        const uint32_t count = std::min(dstBytes / 2, srcBytes);
        for (uint32_t i = 0; i < count; i++)
            dst[i] = uint16_t(src[i]);
        if (written)
            *written = count * 2;
        return X_STATUS_SUCCESS;
    }

    uint32_t RtlUnicodeToMultiByteN(uint8_t* dst, uint32_t dstBytes, u32* written, const u16* src, uint32_t srcBytes)
    {
        const uint32_t count = std::min(dstBytes, srcBytes / 2);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint16_t c = src[i];
            dst[i] = c < 0x100 ? uint8_t(c) : '?';
        }
        if (written)
            *written = count;
        return X_STATUS_SUCCESS;
    }

    uint32_t RtlNtStatusToDosError(uint32_t status)
    {
        switch (status)
        {
        case X_STATUS_SUCCESS: return 0;
        case X_STATUS_TIMEOUT: return 258; // WAIT_TIMEOUT
        case X_STATUS_PENDING: return X_ERROR_IO_PENDING;
        case X_STATUS_BUFFER_OVERFLOW: return 234; // X_ERROR_MORE_DATA
        case X_STATUS_NO_MORE_FILES: return X_ERROR_NO_MORE_FILES;
        case X_STATUS_UNSUCCESSFUL: return 31; // X_ERROR_GEN_FAILURE
        case X_STATUS_NOT_IMPLEMENTED: return 1;
        case X_STATUS_INVALID_HANDLE: return X_ERROR_INVALID_HANDLE;
        case X_STATUS_INVALID_PARAMETER: return X_ERROR_INVALID_PARAMETER;
        case X_STATUS_NO_SUCH_FILE: return X_ERROR_FILE_NOT_FOUND;
        case X_STATUS_END_OF_FILE: return 38; // X_ERROR_HANDLE_EOF
        case X_STATUS_NO_MEMORY: return 8;
        case X_STATUS_ACCESS_DENIED: return X_ERROR_ACCESS_DENIED;
        case X_STATUS_BUFFER_TOO_SMALL: return X_ERROR_INSUFFICIENT_BUFFER;
        case X_STATUS_OBJECT_NAME_INVALID: return 123; // X_ERROR_INVALID_NAME
        case X_STATUS_OBJECT_NAME_NOT_FOUND: return X_ERROR_FILE_NOT_FOUND;
        case X_STATUS_OBJECT_NAME_COLLISION: return X_ERROR_ALREADY_EXISTS;
        case X_STATUS_OBJECT_PATH_NOT_FOUND: return X_ERROR_PATH_NOT_FOUND;
        case X_STATUS_FILE_IS_A_DIRECTORY: return X_ERROR_ACCESS_DENIED;
        case X_STATUS_NOT_A_DIRECTORY: return 267; // X_ERROR_DIRECTORY
        case 0xC0000120: return 995; // X_STATUS_CANCELLED -> X_ERROR_OPERATION_ABORTED
        default:
            break;
        }
        if ((status & 0xFFFF0000) == 0x80070000) // HRESULT_FROM_WIN32
            return status & 0xFFFF;
        LOGW("RtlNtStatusToDosError: unmapped status {:08X}", status);
        return 317; // X_ERROR_MR_MID_NOT_FOUND
    }

    // ---------------------------------------------------------------- time

    struct X_TIME_FIELDS
    {
        u16 year, month, day, hour, minute, second, milliseconds, weekday;
    };

    void RtlTimeToTimeFields(s64* time, X_TIME_FIELDS* fields)
    {
        const uint64_t t = uint64_t(int64_t(*time));
        FILETIME ft{ DWORD(t), DWORD(t >> 32) };
        SYSTEMTIME st{};
        FileTimeToSystemTime(&ft, &st);
        fields->year = st.wYear;
        fields->month = st.wMonth;
        fields->day = st.wDay;
        fields->hour = st.wHour;
        fields->minute = st.wMinute;
        fields->second = st.wSecond;
        fields->milliseconds = st.wMilliseconds;
        fields->weekday = st.wDayOfWeek;
    }

    bool RtlTimeFieldsToTime(X_TIME_FIELDS* fields, s64* time)
    {
        SYSTEMTIME st{};
        st.wYear = fields->year;
        st.wMonth = fields->month;
        st.wDay = fields->day;
        st.wHour = fields->hour;
        st.wMinute = fields->minute;
        st.wSecond = fields->second;
        st.wMilliseconds = fields->milliseconds;
        FILETIME ft;
        if (!SystemTimeToFileTime(&st, &ft))
            return false;
        *time = int64_t((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
        return true;
    }

    // -------------------------------------------------------------- memory

    void RtlFillMemoryUlong(u32* dst, uint32_t length, uint32_t pattern)
    {
        for (uint32_t i = 0; i < length / 4; i++)
            dst[i] = pattern;
    }

    uint32_t RtlCompareMemoryUlong(u32* src, uint32_t length, uint32_t pattern)
    {
        uint32_t i = 0;
        for (; i < length / 4; i++)
        {
            if (src[i] != pattern)
                break;
        }
        return i * 4;
    }

    // -------------------------------------------------------------- printf

    struct ArgReader
    {
        PPCContext& ctx;
        int index;        // register/stack argument index (StackArgList)
        uint32_t array;   // va_list pointer (ArrayArgList), 0 = use registers

        uint64_t Next64()
        {
            if (array)
            {
                const uint64_t v = *g_memory.Translate<be<uint64_t>>(array);
                array += 8;
                return v;
            }
            return bridge::IntArg(ctx, index++);
        }

        uint32_t Next32() { return uint32_t(Next64()); }
    };

    static std::string FormatGuest(const char* format, ArgReader args)
    {
        std::string out;
        for (const char* p = format; *p; p++)
        {
            if (*p != '%')
            {
                out += *p;
                continue;
            }

            const char* specStart = p++;
            if (*p == '%')
            {
                out += '%';
                continue;
            }

            // A width or precision from the arguments, within what fits the buffer below.
            constexpr int32_t kMaxWidth = 511;
            std::string spec = "%";
            while (*p && strchr("-+ #0", *p))
                spec += *p++;

            if (*p == '*')
            {
                spec += std::to_string(std::clamp(int32_t(args.Next32()), -kMaxWidth, kMaxWidth));
                p++;
            }
            else
            {
                while (isdigit(uint8_t(*p)))
                    spec += *p++;
            }

            if (*p == '.')
            {
                spec += *p++;
                if (*p == '*')
                {
                    spec += std::to_string(std::clamp(int32_t(args.Next32()), -kMaxWidth, kMaxWidth));
                    p++;
                }
                else
                {
                    while (isdigit(uint8_t(*p)))
                        spec += *p++;
                }
            }

            enum { Default, Short, Long, LongLong, Wide } size = Default;
            if (*p == 'h') { size = Short; p++; }
            else if (*p == 'l')
            {
                p++;
                size = Long;
                if (*p == 'l') { size = LongLong; p++; }
            }
            else if (*p == 'L' || *p == 'q') { size = LongLong; p++; }
            else if (*p == 'w') { size = Wide; p++; }
            else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { size = LongLong; p += 3; }
            else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { size = Long; p += 3; }
            else if (*p == 'I') { p++; }

            const char conv = *p;
            if (!conv)
            {
                out.append(specStart);
                break;
            }

            char buffer[512] = {}; // (left empty if snprintf fails)
            switch (conv)
            {
            case 'd':
            case 'i':
            {
                if (size == LongLong)
                    snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(), int64_t(args.Next64()));
                else if (size == Short)
                    snprintf(buffer, sizeof(buffer), (spec + "d").c_str(), int16_t(args.Next32()));
                else
                    snprintf(buffer, sizeof(buffer), (spec + "d").c_str(), int32_t(args.Next32()));
                out += buffer;
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o':
            {
                if (size == LongLong)
                    snprintf(buffer, sizeof(buffer), (spec + "ll" + conv).c_str(), uint64_t(args.Next64()));
                else if (size == Short)
                    snprintf(buffer, sizeof(buffer), (spec + conv).c_str(), uint32_t(uint16_t(args.Next32())));
                else
                    snprintf(buffer, sizeof(buffer), (spec + conv).c_str(), args.Next32());
                out += buffer;
                break;
            }
            case 'p':
                snprintf(buffer, sizeof(buffer), "%08X", args.Next32());
                out += buffer;
                break;
            case 'c':
            case 'C':
            {
                const uint32_t c = args.Next32();
                snprintf(buffer, sizeof(buffer), (spec + "c").c_str(), char(c < 0x100 ? c : '?'));
                out += buffer;
                break;
            }
            case 'e':
            case 'E':
            case 'f':
            case 'F':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
            {
                const uint64_t bits = args.Next64();
                double value;
                memcpy(&value, &bits, sizeof(value));
                snprintf(buffer, sizeof(buffer), (spec + conv).c_str(), value);
                out += buffer;
                break;
            }
            case 's':
            case 'S':
            {
                const uint32_t address = args.Next32();
                std::string str;
                const bool wide = (conv == 'S') != (size == Long || size == Wide);
                if (!address)
                {
                    str = "(null)";
                }
                else if (wide)
                {
                    auto* w = g_memory.Translate<u16>(address);
                    for (size_t i = 0; w[i] != 0; i++)
                    {
                        const uint16_t c = w[i];
                        str += c < 0x80 ? char(c) : '?';
                    }
                }
                else
                {
                    str = g_memory.Translate<char>(address);
                }
                // Apply width/precision with the host formatter on a host string.
                std::vector<char> big(str.size() + 512);
                snprintf(big.data(), big.size(), (spec + "s").c_str(), str.c_str());
                out += big.data();
                break;
            }
            case 'n':
            {
                const uint32_t address = args.Next32();
                if (address)
                    *g_memory.Translate<u32>(address) = uint32_t(out.size());
                break;
            }
            default:
                out.append(specStart, p + 1);
                break;
            }
        }
        return out;
    }

    // Writes like MSVC's _vsnprintf: returns -1 (without terminating) on truncation.
    static int32_t WriteBounded(char* buffer, uint32_t count, const std::string& text)
    {
        if (text.size() > count)
        {
            memcpy(buffer, text.data(), count);
            return -1;
        }
        memcpy(buffer, text.data(), text.size());
        if (text.size() < count)
            buffer[text.size()] = 0;
        return int32_t(text.size());
    }

    int32_t sprintf(PPCContext& ctx, char* buffer, const char* format)
    {
        if (!buffer || !format)
            return -1;
        const std::string text = FormatGuest(format, { ctx, 2, 0 });
        memcpy(buffer, text.c_str(), text.size() + 1);
        return int32_t(text.size());
    }

    int32_t _vsnprintf(PPCContext& ctx, char* buffer, uint32_t count, const char* format, uint32_t vaList)
    {
        if (!format)
            return -1;
        const std::string text = FormatGuest(format, { ctx, 0, vaList });
        return buffer ? WriteBounded(buffer, count, text) : int32_t(text.size());
    }

    uint32_t DbgPrint(PPCContext& ctx, const char* format)
    {
        if (format)
        {
            std::string text = FormatGuest(format, { ctx, 1, 0 });
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            LOGI("DbgPrint: {}", text);
        }
        return X_STATUS_SUCCESS;
    }

    // --------------------------------------------------------------- debug

    void DbgBreakPoint()
    {
        LOGW("DbgBreakPoint");
    }

    [[noreturn]] static void BugCheck(uint32_t code, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
    {
        LOGE("KeBugCheck {:08X} ({:08X}, {:08X}, {:08X}, {:08X})", code, a, b, c, d);
        if (IsDebuggerPresent())
            __debugbreak();
        TerminateProcess(GetCurrentProcess(), 0xBC000000 | (code & 0xFFFF));
        std::abort();
    }

    void KeBugCheck(uint32_t code) { BugCheck(code, 0, 0, 0, 0); }
    void KeBugCheckEx(uint32_t code, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { BugCheck(code, a, b, c, d); }

    struct X_EXCEPTION_RECORD
    {
        u32 code;
        u32 flags;
        u32 record;
        u32 address;
        u32 parameterCount;
        u32 information[15];
    };

    void RtlRaiseException(X_EXCEPTION_RECORD* record)
    {
        const uint32_t code = record->code;
        if (code == 0x406D1388 && record->parameterCount >= 3) // MSVC SetThreadName
        {
            const uint32_t nameAddress = record->information[1];
            const uint32_t threadId = record->information[2];
            const char* name = nameAddress ? g_memory.Translate<char>(nameAddress) : "";
            LOGI("Thread name: {} -> '{}'", int32_t(threadId), name);
            auto* self = GuestThread::Current();
            if (self && (threadId == 0xFFFFFFFF || threadId == self->threadId))
            {
                std::wstring wide(name, name + strlen(name));
                SetThreadDescription(GetCurrentThread(), wide.c_str());
            }
            return;
        }

        LOGE("RtlRaiseException {:08X} flags {:#x} at {:08X} ({} params)", code, uint32_t(record->flags),
            uint32_t(record->address), uint32_t(record->parameterCount));
        for (uint32_t i = 0; i < std::min<uint32_t>(record->parameterCount, 15); i++)
            LOGE("  param[{}] = {:08X}", i, uint32_t(record->information[i]));

        if (code == 0xE06D7363) // C++ exception
            LOGE("Guest C++ exceptions are not supported by the recompiled runtime");

        if (uint32_t(record->flags) & 1) // EXCEPTION_NONCONTINUABLE
            BugCheck(0xDEAD0001, code, record->address, 0, 0);
    }

    void RtlUnwind(uint32_t targetFrame, uint32_t targetIp, uint32_t record, uint32_t returnValue)
    {
        LOGE("RtlUnwind({:08X}, {:08X}, {:08X}, {:08X}) is not supported", targetFrame, targetIp, record, returnValue);
    }

    void RtlCaptureContext(uint32_t context)
    {
        // CONTEXT is only consumed by unwinding/diagnostics, which we don't support.
        if (context)
            memset(g_memory.Translate(context), 0, 0x200);
    }

    uint32_t __C_specific_handler(uint32_t, uint32_t, uint32_t, uint32_t)
    {
        return 1; // ExceptionContinueSearch
    }

    // -------------------------------------------------------------- modules

    uint32_t RtlImageXexHeaderField(uint32_t header, uint32_t key)
    {
        return XexHeaderField(header, key);
    }

    uint32_t XexGetModuleHandle(const char* name, u32* handleOut)
    {
        if (!handleOut)
            return X_STATUS_INVALID_PARAMETER;
        if (!name || !_stricmp(name, "default.xex"))
        {
            *handleOut = g_module.ldrEntry;
            return X_STATUS_SUCCESS;
        }
        LOGW("XexGetModuleHandle('{}'): module not found", name);
        *handleOut = 0;
        return X_STATUS_NO_SUCH_FILE;
    }

    uint32_t XexGetProcedureAddress(uint32_t module, uint32_t ordinal, u32* addressOut)
    {
        LOGW("XexGetProcedureAddress({:08X}, {:#x}) unsupported", module, ordinal);
        if (addressOut)
            *addressOut = 0;
        return X_STATUS_NOT_IMPLEMENTED;
    }

    uint32_t XexCheckExecutablePrivilege(uint32_t privilege)
    {
        if (privilege >= 32)
            return 0;
        return (g_module.systemFlags >> privilege) & 1;
    }

    void ExRegisterTitleTerminateNotification(uint32_t registration, bool create) {}

    void HalReturnToFirmware(uint32_t routine)
    {
        LOGI("HalReturnToFirmware({}) - exiting", routine);
        TerminateProcess(GetCurrentProcess(), 0);
    }

    // ------------------------------------------------------------- XConfig

    uint32_t ExGetXConfigSetting(uint16_t category, uint16_t setting, uint8_t* buffer, uint16_t bufferSize,
        u16* requiredSize)
    {
        uint8_t value[4] = {};
        uint16_t size = 4;
        auto set32 = [&](uint32_t v) { *reinterpret_cast<be<uint32_t>*>(value) = v; };

        switch (category)
        {
        case 0x02: // secured
            switch (setting)
            {
            case 0x02: set32(0x00001000); break; // AV region: NTSC-M
            default: goto unknown;
            }
            break;
        case 0x03: // user
            switch (setting)
            {
            case 0x01: set32(0); break;                // time zone bias
            case 0x09: set32(1); break;                // language: English
            case 0x0A: set32(0x00040000); break;       // video flags: widescreen
            case 0x0B: set32(0x00010001); break;       // audio flags: stereo
            case 0x0C: set32(0x40); break;             // retail flags
            case 0x0E: size = 1; value[0] = 103; break; // country: US
            case 0x0F: size = 1; value[0] = 0; break;  // parental control flags
            default: goto unknown;
            }
            break;
        default:
        unknown:
            LOGW("ExGetXConfigSetting({:#x}, {:#x}) unknown", category, setting);
            return X_STATUS_INVALID_PARAMETER;
        }

        if (buffer)
        {
            if (bufferSize < size)
                return X_STATUS_BUFFER_TOO_SMALL;
            memcpy(buffer, value, size);
        }
        if (requiredSize)
            *requiredSize = size;
        return X_STATUS_SUCCESS;
    }
}

DAO_IMPORT_AS(RtlInitAnsiString, dao::imp::RtlInitAnsiString)
DAO_IMPORT_AS(RtlInitUnicodeString, dao::imp::RtlInitUnicodeString)
DAO_IMPORT_AS(RtlFreeAnsiString, dao::imp::RtlFreeAnsiString)
DAO_IMPORT_AS(RtlUnicodeStringToAnsiString, dao::imp::RtlUnicodeStringToAnsiString)
DAO_IMPORT_AS(RtlMultiByteToUnicodeN, dao::imp::RtlMultiByteToUnicodeN)
DAO_IMPORT_AS(RtlUnicodeToMultiByteN, dao::imp::RtlUnicodeToMultiByteN)
DAO_IMPORT_AS(RtlNtStatusToDosError, dao::imp::RtlNtStatusToDosError)
DAO_IMPORT_AS(RtlTimeToTimeFields, dao::imp::RtlTimeToTimeFields)
DAO_IMPORT_AS(RtlTimeFieldsToTime, dao::imp::RtlTimeFieldsToTime)
DAO_IMPORT_AS(RtlFillMemoryUlong, dao::imp::RtlFillMemoryUlong)
DAO_IMPORT_AS(RtlCompareMemoryUlong, dao::imp::RtlCompareMemoryUlong)
DAO_IMPORT_AS(sprintf, dao::imp::sprintf)
DAO_IMPORT_AS(_vsnprintf, dao::imp::_vsnprintf)
DAO_IMPORT_AS(DbgPrint, dao::imp::DbgPrint)
DAO_IMPORT_AS(DbgBreakPoint, dao::imp::DbgBreakPoint)
DAO_IMPORT_AS(KeBugCheck, dao::imp::KeBugCheck)
DAO_IMPORT_AS(KeBugCheckEx, dao::imp::KeBugCheckEx)
DAO_IMPORT_AS(RtlRaiseException, dao::imp::RtlRaiseException)
DAO_IMPORT_AS(RtlUnwind, dao::imp::RtlUnwind)
DAO_IMPORT_AS(RtlCaptureContext, dao::imp::RtlCaptureContext)
DAO_IMPORT_AS(__C_specific_handler, dao::imp::__C_specific_handler)
DAO_IMPORT_AS(RtlImageXexHeaderField, dao::imp::RtlImageXexHeaderField)
DAO_IMPORT_AS(XexGetModuleHandle, dao::imp::XexGetModuleHandle)
DAO_IMPORT_AS(XexGetProcedureAddress, dao::imp::XexGetProcedureAddress)
DAO_IMPORT_AS(XexCheckExecutablePrivilege, dao::imp::XexCheckExecutablePrivilege)
DAO_IMPORT_AS(ExRegisterTitleTerminateNotification, dao::imp::ExRegisterTitleTerminateNotification)
DAO_IMPORT_AS(HalReturnToFirmware, dao::imp::HalReturnToFirmware)
DAO_IMPORT_AS(ExGetXConfigSetting, dao::imp::ExGetXConfigSetting)
